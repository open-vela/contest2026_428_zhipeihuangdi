#include "NetworkTest.hpp"

#include <cstdio>
#include <cstdlib>
#include <cstring>

#include "cJSON.h"
#include "esp_crt_bundle.h"
#include "esp_http_client.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "lwip/dns.h"
#include "lwip/ip_addr.h"
#include "bsp_board_extra.h"
#include "bsp/esp-bsp.h"
#include "audio_player.h"
#include "api_key_store.h"
#include "streaming/StreamOpusEncoder.hpp"
#include "streaming/StreamProtocol.hpp"
#include "vela_eyes/eye/eye.h"
#include "stm32_comm.h"

LV_IMG_DECLARE(img_app_setting);
LV_FONT_DECLARE(vela_font_chinese_18);
extern "C" lv_pinyin_dict_t vela_pinyin_dict[];

namespace {
constexpr const char *TAG = "api_test";
constexpr const char *API_URL = "http://ip-api.com/json/?lang=en";
constexpr const char *HTTPS_URL = "https://httpbin.org/json";
constexpr const char *DEEPSEEK_URL = "https://api.deepseek.com/chat/completions";
constexpr const char *SPEECH_URL = "https://api.siliconflow.cn/v1/audio/transcriptions";
constexpr const char *TTS_URL = "https://api.siliconflow.cn/v1/audio/speech";
constexpr const char *TTS_FILE = BSP_SPIFFS_MOUNT_POINT "/vela_tts.mp3";
// Direct cloud ASR uses local VAD, so recording length follows the speaker.
constexpr size_t SPEECH_MAX_MILLISECONDS = 12000;
constexpr size_t SPEECH_SAMPLE_RATE = 16000;
constexpr size_t SPEECH_CAPTURE_CHANNELS = 2;
constexpr size_t SPEECH_WAV_CHANNELS = 1;
constexpr size_t SPEECH_BITS = 16;
constexpr size_t SPEECH_MAX_PCM_BYTES = SPEECH_MAX_MILLISECONDS * SPEECH_SAMPLE_RATE *
                                        SPEECH_WAV_CHANNELS * (SPEECH_BITS / 8) / 1000;
constexpr uint32_t VAD_MIN_LEVEL = 250;

struct HttpBuffer {
    char data[4096];
    size_t length;
    bool overflow;
};

struct TtsDownload {
    FILE *file;
    size_t length;
    bool failed;
    int64_t last_data_us;
    int64_t started_us;
};

// Keep one HTTPS client for the lifetime of the application. Reusing the TLS
// connection avoids repeatedly driving the ESP-Hosted SDIO transport through
// a full certificate handshake on every button press.
static HttpBuffer s_https_buffer = {};
static esp_http_client_handle_t s_https_client = nullptr;
static HttpBuffer s_deepseek_buffer = {};
static esp_http_client_handle_t s_deepseek_client = nullptr;
static HttpBuffer s_http_buffer = {};
static HttpBuffer s_speech_buffer = {};

// TTS download timeout constants
constexpr int64_t TTS_IDLE_TIMEOUT_US = 4000000;   // 4s no data → failed
constexpr int64_t TTS_TOTAL_TIMEOUT_US = 10000000; // 10s total → failed

void writeLe16(uint8_t *target, uint16_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
}

void writeLe32(uint8_t *target, uint32_t value)
{
    target[0] = value & 0xff;
    target[1] = (value >> 8) & 0xff;
    target[2] = (value >> 16) & 0xff;
    target[3] = (value >> 24) & 0xff;
}

void makeWavHeader(uint8_t *header, uint32_t pcm_size)
{
    memcpy(header, "RIFF", 4);
    writeLe32(header + 4, pcm_size + 36);
    memcpy(header + 8, "WAVEfmt ", 8);
    writeLe32(header + 16, 16);
    writeLe16(header + 20, 1);
    writeLe16(header + 22, SPEECH_WAV_CHANNELS);
    writeLe32(header + 24, SPEECH_SAMPLE_RATE);
    writeLe32(header + 28, SPEECH_SAMPLE_RATE * SPEECH_WAV_CHANNELS * (SPEECH_BITS / 8));
    writeLe16(header + 32, SPEECH_WAV_CHANNELS * (SPEECH_BITS / 8));
    writeLe16(header + 34, SPEECH_BITS);
    memcpy(header + 36, "data", 4);
    writeLe32(header + 40, pcm_size);
}

esp_err_t httpClientEvent(esp_http_client_event_t *event)
{
    auto *buffer = static_cast<HttpBuffer *>(event->user_data);
    if ((event->event_id != HTTP_EVENT_ON_DATA) || !buffer || !event->data ||
        (event->data_len <= 0)) return ESP_OK;

    const size_t available = sizeof(buffer->data) - buffer->length - 1;
    const size_t incoming = static_cast<size_t>(event->data_len);
    const size_t copied = incoming < available ? incoming : available;
    if (copied > 0) {
        memcpy(buffer->data + buffer->length, event->data, copied);
        buffer->length += copied;
        buffer->data[buffer->length] = '\0';
    }
    if (copied != incoming) buffer->overflow = true;
    return ESP_OK;
}

esp_err_t ttsClientEvent(esp_http_client_event_t *event)
{
    auto *download = static_cast<TtsDownload *>(event->user_data);
    if ((event->event_id != HTTP_EVENT_ON_DATA) || !download || !download->file ||
        !event->data || (event->data_len <= 0)) return ESP_OK;
    const int64_t now = esp_timer_get_time();
    if ((now - download->last_data_us) > TTS_IDLE_TIMEOUT_US ||
        (now - download->started_us) > TTS_TOTAL_TIMEOUT_US) {
        download->failed = true;
        return ESP_FAIL;
    }
    download->last_data_us = now;
    const size_t written = fwrite(event->data, 1, event->data_len, download->file);
    download->length += written;
    if (written != static_cast<size_t>(event->data_len)) download->failed = true;
    return ESP_OK;
}

const char *jsonText(cJSON *object, const char *key, const char *fallback)
{
    cJSON *item = cJSON_GetObjectItemCaseSensitive(object, key);
    return cJSON_IsString(item) && item->valuestring ? item->valuestring : fallback;
}

// Keep the first spoken response short enough for low-latency synthesis while
// leaving the complete AI answer untouched on screen and in conversation
// history. Prefer a natural sentence boundary and never split a UTF-8 codepoint.
void makeFastTtsText(const char *source, char *target, size_t target_size)
{
    if (!source || !target || target_size == 0) return;
    constexpr size_t MAX_TTS_BYTES = 210;
    constexpr size_t MIN_SENTENCE_BYTES = 36;
    size_t read = 0;
    size_t written = 0;
    while (source[read] != '\0' && written < MAX_TTS_BYTES && written + 1 < target_size) {
        const uint8_t lead = static_cast<uint8_t>(source[read]);
        size_t char_size = 1;
        if ((lead & 0xE0) == 0xC0) char_size = 2;
        else if ((lead & 0xF0) == 0xE0) char_size = 3;
        else if ((lead & 0xF8) == 0xF0) char_size = 4;
        if (written + char_size > MAX_TTS_BYTES || written + char_size >= target_size) break;
        memcpy(target + written, source + read, char_size);
        written += char_size;
        read += char_size;
        target[written] = '\0';

        const bool ascii_stop = (char_size == 1) &&
            ((lead == '.') || (lead == '!') || (lead == '?') || (lead == ';'));
        const bool chinese_stop = (char_size == 3) &&
            ((memcmp(target + written - 3, "。", 3) == 0) ||
             (memcmp(target + written - 3, "！", 3) == 0) ||
             (memcmp(target + written - 3, "？", 3) == 0) ||
             (memcmp(target + written - 3, "；", 3) == 0));
        if (written >= MIN_SENTENCE_BYTES && (ascii_stop || chinese_stop)) break;
    }
    target[written] = '\0';
}
}  // namespace

struct NetworkTest::UiResult {
    NetworkTest *app;
    bool success;
    bool deepseek;
    bool speech;
    bool tts;
    bool cancelled;
    char title[32];
    char body[512];
};

NetworkTest::NetworkTest():
    // Robot mode uses the entire LCD. Keep the flexible navigation gesture so
    // the launcher/settings remain reachable, but do not reserve a status bar.
    ESP_Brookesia_PhoneApp("Vela AI", &img_app_setting, true, false, true)
{
}

NetworkTest::~NetworkTest() = default;

bool NetworkTest::init(void)
{
    return true;
}

bool NetworkTest::run(void)
{
    // The default application screen is already sized to the visual area by
    // Brookesia. Applying getVisualArea().x1/y1 again created the blank strip
    // seen above the eyes, so fill the active screen from its local origin.
    const lv_coord_t width = lv_obj_get_width(lv_scr_act());
    const lv_coord_t height = lv_obj_get_height(lv_scr_act());
    root_ = lv_obj_create(lv_scr_act());
    lv_obj_set_pos(root_, 0, 0);
    lv_obj_set_size(root_, width, height);
    lv_obj_set_style_bg_color(root_, lv_color_hex(0x34383D), 0);
    lv_obj_set_style_border_width(root_, 0, 0);
    lv_obj_set_style_radius(root_, 0, 0);
    lv_obj_set_style_pad_all(root_, 0, 0);
    lv_obj_clear_flag(root_, LV_OBJ_FLAG_SCROLLABLE);

    eye_init(root_);

    // Three soft, nested borders create a calm Codex-like listening glow.
    lv_obj_t **glows[] = {&glow_outer_, &glow_middle_, &glow_inner_};
    const int insets[] = {2, 8, 15};
    const int widths[] = {5, 7, 9};
    for (int i = 0; i < 3; ++i) {
        *glows[i] = lv_obj_create(root_);
        lv_obj_set_pos(*glows[i], insets[i], insets[i]);
        lv_obj_set_size(*glows[i], width - insets[i] * 2, height - insets[i] * 2);
        lv_obj_set_style_bg_opa(*glows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_border_width(*glows[i], widths[i], 0);
        lv_obj_set_style_border_color(*glows[i], lv_color_hex(0x61D5FF), 0);
        lv_obj_set_style_border_opa(*glows[i], LV_OPA_TRANSP, 0);
        lv_obj_set_style_radius(*glows[i], 24, 0);
        lv_obj_clear_flag(*glows[i], LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_add_flag(*glows[i], LV_OBJ_FLAG_HIDDEN);
    }

    status_label_ = lv_label_create(root_);
    lv_obj_set_width(status_label_, lv_pct(100));
    lv_obj_set_style_text_align(status_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(status_label_, &vela_font_chinese_18, 0);
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x91E8F8), 0);
    lv_label_set_text(status_label_, "可以直接说话");
    lv_obj_align(status_label_, LV_ALIGN_BOTTOM_MID, 0, -18);

    prompt_textarea_ = lv_textarea_create(root_);
    lv_obj_set_size(prompt_textarea_, lv_pct(82), 68);
    lv_obj_align(prompt_textarea_, LV_ALIGN_TOP_MID, 0, 82);
    lv_textarea_set_one_line(prompt_textarea_, true);
    lv_textarea_set_max_length(prompt_textarea_, sizeof(prompt_) - 1);
    lv_textarea_set_placeholder_text(prompt_textarea_, "请输入问题...");
    lv_textarea_set_text(prompt_textarea_, prompt_);
    lv_obj_set_style_text_font(prompt_textarea_, &vela_font_chinese_18, 0);
    lv_obj_add_event_cb(prompt_textarea_, promptEvent, LV_EVENT_FOCUSED, this);
    lv_obj_add_flag(prompt_textarea_, LV_OBJ_FLAG_HIDDEN);

    result_label_ = lv_label_create(root_);
    lv_obj_set_width(result_label_, lv_pct(88));
    lv_obj_set_style_text_align(result_label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_style_text_font(result_label_, &vela_font_chinese_18, 0);
    lv_obj_set_style_text_color(result_label_, lv_color_white(), 0);
    lv_label_set_long_mode(result_label_, LV_LABEL_LONG_WRAP);
    lv_label_set_text(result_label_, "连接网络后，可以用中文向 DeepSeek 提问");
    lv_obj_align(result_label_, LV_ALIGN_CENTER, 0, 28);
    lv_obj_add_flag(result_label_, LV_OBJ_FLAG_HIDDEN);

    http_button_ = lv_btn_create(root_);
    lv_obj_set_size(http_button_, 160, 62);
    lv_obj_align(http_button_, LV_ALIGN_BOTTOM_MID, -270, -40);
    lv_obj_add_event_cb(http_button_, httpEvent, LV_EVENT_CLICKED, this);
    lv_obj_t *http_label = lv_label_create(http_button_);
    lv_label_set_text(http_label, "Test HTTP");
    lv_obj_center(http_label);
    lv_obj_add_flag(http_button_, LV_OBJ_FLAG_HIDDEN);

    https_button_ = lv_btn_create(root_);
    lv_obj_set_size(https_button_, 160, 62);
    lv_obj_align(https_button_, LV_ALIGN_BOTTOM_MID, -90, -40);
    lv_obj_add_event_cb(https_button_, httpsEvent, LV_EVENT_CLICKED, this);
    lv_obj_t *https_label = lv_label_create(https_button_);
    lv_label_set_text(https_label, "Test HTTPS");
    lv_obj_center(https_label);
    lv_obj_add_flag(https_button_, LV_OBJ_FLAG_HIDDEN);

    deepseek_button_ = lv_btn_create(root_);
    lv_obj_set_size(deepseek_button_, 160, 62);
    lv_obj_align(deepseek_button_, LV_ALIGN_BOTTOM_MID, 270, -40);
    lv_obj_add_event_cb(deepseek_button_, deepSeekEvent, LV_EVENT_CLICKED, this);
    lv_obj_t *deepseek_label = lv_label_create(deepseek_button_);
    lv_label_set_text(deepseek_label, "Ask DeepSeek");
    lv_obj_center(deepseek_label);
    lv_obj_add_flag(deepseek_button_, LV_OBJ_FLAG_HIDDEN);

    voice_button_ = lv_btn_create(root_);
    mic_button_ = voice_button_;
    lv_obj_set_size(voice_button_, 84, 84);
    lv_obj_align(voice_button_, LV_ALIGN_BOTTOM_MID, 0, -62);
    lv_obj_set_style_radius(voice_button_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(voice_button_, lv_color_hex(0xE9F4F7), 0);
    lv_obj_set_style_bg_color(voice_button_, lv_color_hex(0xBDEBFA), LV_STATE_PRESSED);
    lv_obj_set_style_shadow_color(voice_button_, lv_color_hex(0x76D7F2), 0);
    lv_obj_set_style_shadow_width(voice_button_, 18, 0);
    lv_obj_set_style_shadow_opa(voice_button_, LV_OPA_30, 0);
    lv_obj_add_event_cb(voice_button_, voiceEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(voice_button_, streamTestEvent, LV_EVENT_LONG_PRESSED, this);
    lv_obj_t *mic_capsule = lv_obj_create(voice_button_);
    lv_obj_set_size(mic_capsule, 20, 34);
    lv_obj_align(mic_capsule, LV_ALIGN_CENTER, 0, -6);
    lv_obj_set_style_radius(mic_capsule, 10, 0);
    lv_obj_set_style_bg_color(mic_capsule, lv_color_hex(0x263A43), 0);
    lv_obj_set_style_border_width(mic_capsule, 0, 0);
    lv_obj_clear_flag(mic_capsule, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mic_stem = lv_obj_create(voice_button_);
    lv_obj_set_size(mic_stem, 4, 16);
    lv_obj_align(mic_stem, LV_ALIGN_CENTER, 0, 18);
    lv_obj_set_style_radius(mic_stem, 2, 0);
    lv_obj_set_style_bg_color(mic_stem, lv_color_hex(0x263A43), 0);
    lv_obj_set_style_border_width(mic_stem, 0, 0);
    lv_obj_clear_flag(mic_stem, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t *mic_base = lv_obj_create(voice_button_);
    lv_obj_set_size(mic_base, 26, 4);
    lv_obj_align(mic_base, LV_ALIGN_CENTER, 0, 26);
    lv_obj_set_style_radius(mic_base, 2, 0);
    lv_obj_set_style_bg_color(mic_base, lv_color_hex(0x263A43), 0);
    lv_obj_set_style_border_width(mic_base, 0, 0);
    lv_obj_clear_flag(mic_base, LV_OBJ_FLAG_CLICKABLE | LV_OBJ_FLAG_SCROLLABLE);

    speak_button_ = lv_btn_create(root_);
    lv_obj_set_size(speak_button_, 180, 56);
    lv_obj_align(speak_button_, LV_ALIGN_BOTTOM_MID, 0, -112);
    lv_obj_add_event_cb(speak_button_, speakEvent, LV_EVENT_CLICKED, this);
    lv_obj_add_state(speak_button_, LV_STATE_DISABLED);
    lv_obj_t *speak_label = lv_label_create(speak_button_);
    lv_obj_set_style_text_font(speak_label, &vela_font_chinese_18, 0);
    lv_label_set_text(speak_label, "朗读回答");
    lv_obj_center(speak_label);
    lv_obj_add_flag(speak_button_, LV_OBJ_FLAG_HIDDEN);

    active_ = true;
    interaction_.reset();
    setInteractionState(RobotInteractionState::Listening);
    scheduleAutoListen(1200);
    return true;
}

bool NetworkTest::back(void)
{
    notifyCoreClosed();
    return true;
}

bool NetworkTest::close(void)
{
    active_ = false;
    if (auto_listen_timer_) {
        lv_timer_del(auto_listen_timer_);
        auto_listen_timer_ = nullptr;
    }
    setListeningGlow(false);
    eye_deinit();
    root_ = nullptr;
    status_label_ = nullptr;
    result_label_ = nullptr;
    http_button_ = nullptr;
    https_button_ = nullptr;
    deepseek_button_ = nullptr;
    voice_button_ = nullptr;
    speak_button_ = nullptr;
    prompt_textarea_ = nullptr;
    keyboard_ = nullptr;
    pinyin_ime_ = nullptr;
    glow_outer_ = nullptr;
    glow_middle_ = nullptr;
    glow_inner_ = nullptr;
    mic_button_ = nullptr;
    return true;
}

void NetworkTest::httpEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (app) app->startRequest(RequestType::Http);
}

void NetworkTest::httpsEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (app) app->startRequest(RequestType::Https);
}

void NetworkTest::deepSeekEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (app) app->startRequest(RequestType::DeepSeek);
}

void NetworkTest::voiceEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (!app) return;
    if (app->listening_active_) {
        app->finishListening();
    } else {
        // A normal tap starts the streaming transport self-test.
        app->startStreamTest();
    }
}

void NetworkTest::streamTestEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (app) app->startStreamTest();
}

void NetworkTest::speakEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (app) app->startTts();
}

void NetworkTest::promptEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (!app || !app->active_) return;
    app->createKeyboard();
    if (!app->keyboard_) return;
    lv_obj_move_foreground(app->keyboard_);
    if (app->pinyin_ime_) {
        lv_obj_t *candidate_panel = lv_ime_pinyin_get_cand_panel(app->pinyin_ime_);
        if (candidate_panel) lv_obj_move_foreground(candidate_panel);
    }
    lv_label_set_text(app->status_label_, "拼音输入：输入字母后选择候选字");
}

void NetworkTest::keyboardEvent(lv_event_t *event)
{
    auto *app = static_cast<NetworkTest *>(lv_event_get_user_data(event));
    if (!app || !app->keyboard_) return;
    const lv_event_code_t code = lv_event_get_code(event);
    lv_obj_clear_state(app->prompt_textarea_, LV_STATE_FOCUSED);
    app->releaseKeyboard();

    // Let LVGL finish deleting the large keyboard object before TLS starts.
    // Keeping a hidden keyboard alive during the handshake caused memory
    // pressure and a blue-screen crash on the P4 board.
    if (code == LV_EVENT_READY) {
        lv_timer_t *timer = lv_timer_create(delayedDeepSeek, 200, app);
        if (timer) lv_timer_set_repeat_count(timer, 1);
    }
}

void NetworkTest::createKeyboard()
{
    if (!root_ || !prompt_textarea_) return;
    if (keyboard_) {
        lv_obj_clear_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
        if (pinyin_ime_) {
            lv_obj_t *candidate_panel = lv_ime_pinyin_get_cand_panel(pinyin_ime_);
            if (candidate_panel) lv_obj_clear_flag(candidate_panel, LV_OBJ_FLAG_HIDDEN);
        }
        return;
    }
    keyboard_ = lv_keyboard_create(root_);
    if (!keyboard_) return;
    lv_obj_set_size(keyboard_, lv_pct(100), lv_pct(48));
    lv_obj_align(keyboard_, LV_ALIGN_BOTTOM_MID, 0, 0);
    lv_keyboard_set_textarea(keyboard_, prompt_textarea_);
    if (!pinyin_ime_) {
        pinyin_ime_ = lv_ime_pinyin_create(root_);
        lv_obj_set_style_text_font(pinyin_ime_, &vela_font_chinese_18, 0);
        lv_ime_pinyin_set_dict(pinyin_ime_, vela_pinyin_dict);
    }
    lv_ime_pinyin_set_mode(pinyin_ime_, LV_IME_PINYIN_MODE_K26);
    lv_ime_pinyin_set_keyboard(pinyin_ime_, keyboard_);
    lv_obj_t *candidate_panel = lv_ime_pinyin_get_cand_panel(pinyin_ime_);
    if (candidate_panel) {
        lv_obj_set_style_text_font(candidate_panel, &vela_font_chinese_18, 0);
        lv_obj_set_style_bg_color(candidate_panel, lv_color_white(), LV_PART_MAIN);
        lv_obj_set_style_bg_opa(candidate_panel, LV_OPA_COVER, LV_PART_MAIN);
        lv_obj_set_style_border_color(candidate_panel, lv_color_hex(0xA8B0BA), LV_PART_MAIN);
        lv_obj_set_style_border_width(candidate_panel, 1, LV_PART_MAIN);
        lv_obj_set_style_text_color(candidate_panel, lv_color_black(), LV_PART_ITEMS);
        lv_obj_set_style_bg_color(candidate_panel, lv_color_hex(0xEEF1F4), LV_PART_ITEMS);
        lv_obj_set_style_bg_opa(candidate_panel, LV_OPA_COVER, LV_PART_ITEMS);
        lv_obj_set_style_border_color(candidate_panel, lv_color_hex(0xC8CDD3), LV_PART_ITEMS);
        lv_obj_set_style_border_width(candidate_panel, 1, LV_PART_ITEMS);
        lv_obj_set_size(candidate_panel, lv_pct(100), 48);
        lv_obj_align_to(candidate_panel, keyboard_, LV_ALIGN_OUT_TOP_MID, 0, 0);
        lv_obj_move_foreground(candidate_panel);
    }
    lv_obj_add_event_cb(keyboard_, keyboardEvent, LV_EVENT_READY, this);
    lv_obj_add_event_cb(keyboard_, keyboardEvent, LV_EVENT_CANCEL, this);
}

void NetworkTest::releaseKeyboard()
{
    if (!keyboard_) return;
    lv_obj_add_flag(keyboard_, LV_OBJ_FLAG_HIDDEN);
    if (pinyin_ime_) {
        lv_obj_t *candidate_panel = lv_ime_pinyin_get_cand_panel(pinyin_ime_);
        if (candidate_panel) lv_obj_add_flag(candidate_panel, LV_OBJ_FLAG_HIDDEN);
    }
}

void NetworkTest::delayedDeepSeek(lv_timer_t *timer)
{
    auto *app = static_cast<NetworkTest *>(timer->user_data);
    if (app && app->active_) app->startRequest(RequestType::DeepSeek);
}

void NetworkTest::delayedTts(lv_timer_t *timer)
{
    auto *app = static_cast<NetworkTest *>(timer->user_data);
    if (app && app->active_) app->startTts();
}

void NetworkTest::autoListenTimer(lv_timer_t *timer)
{
    auto *app = static_cast<NetworkTest *>(timer->user_data);
    if (!app || !app->active_ || !app->auto_listen_enabled_) return;
    if (app->request_task_) return;

    const audio_player_state_t player_state = audio_player_get_state();
    if ((player_state == AUDIO_PLAYER_STATE_PLAYING) ||
        (player_state == AUDIO_PLAYER_STATE_PAUSE)) {
        return;
    }

    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        if (app->status_label_) lv_label_set_text(app->status_label_, "等待 Wi-Fi 连接…");
        return;
    }

    app->auto_listen_timer_ = nullptr;
    lv_timer_del(timer);
    app->startStreamTest();
}

void NetworkTest::scheduleAutoListen(uint32_t delay_ms)
{
    if (!active_ || !auto_listen_enabled_) return;
    if (auto_listen_timer_) {
        lv_timer_del(auto_listen_timer_);
        auto_listen_timer_ = nullptr;
    }
    auto_listen_timer_ = lv_timer_create(autoListenTimer, delay_ms, this);
    if (auto_listen_timer_) {
        // Keep checking at the requested interval if Wi-Fi/audio is not ready.
        lv_timer_set_repeat_count(auto_listen_timer_, -1);
    }
}

void NetworkTest::setInteractionState(RobotInteractionState state)
{
    // The behavior layer only emits states. Future expression artwork only
    // needs to replace this one mapping, not any voice or network code.
    switch (state) {
    case RobotInteractionState::AwaitingWake:
        eye_set_state(EYE_HAPPY);
        stm32_send_pan_tilt(90, 90);
        break;
    case RobotInteractionState::Listening:
        eye_set_state(EYE_HAPPY);
        stm32_send_pan_tilt(90, 90);
        break;
    case RobotInteractionState::Thinking:
        eye_set_state(EYE_CURIOUS);
        stm32_send_pan_tilt(110, 115);
        break;
    case RobotInteractionState::Speaking:
        eye_set_state(EYE_SURPRISED);
        stm32_send_pan_tilt(90, 60);
        break;
    case RobotInteractionState::Error:
        eye_set_state(EYE_SAD);
        stm32_send_pan_tilt(90, 90);
        break;
    }
}

void NetworkTest::listeningGlowTimer(lv_timer_t *timer)
{
    auto *app = static_cast<NetworkTest *>(timer->user_data);
    if (!app || !app->active_) return;
    app->glow_phase_ = (app->glow_phase_ + 2) % 360;
    lv_obj_t *glows[] = {app->glow_outer_, app->glow_middle_, app->glow_inner_};
    const uint16_t hue_offsets[] = {0, 42, 82};
    const lv_opa_t base_opacity[] = {95, 62, 34};
    for (int i = 0; i < 3; ++i) {
        if (!glows[i]) continue;
        const uint16_t hue = (app->glow_phase_ + hue_offsets[i]) % 360;
        const uint16_t wave = app->glow_phase_ < 180 ? app->glow_phase_ : 360 - app->glow_phase_;
        lv_obj_set_style_border_color(glows[i], lv_color_hsv_to_rgb(hue, 48, 100), 0);
        lv_obj_set_style_border_opa(glows[i], base_opacity[i] + wave / 6, 0);
    }
}

void NetworkTest::setListeningGlow(bool enabled)
{
    lv_obj_t *glows[] = {glow_outer_, glow_middle_, glow_inner_};
    for (lv_obj_t *glow : glows) {
        if (!glow) continue;
        if (enabled) lv_obj_clear_flag(glow, LV_OBJ_FLAG_HIDDEN);
        else lv_obj_add_flag(glow, LV_OBJ_FLAG_HIDDEN);
    }
    if (enabled && !glow_timer_) {
        glow_phase_ = 190;
        glow_timer_ = lv_timer_create(listeningGlowTimer, 40, this);
    } else if (!enabled && glow_timer_) {
        lv_timer_del(glow_timer_);
        glow_timer_ = nullptr;
    }
}

void NetworkTest::startSpeechRecognition()
{
    if (!active_ || request_task_) return;
    char speech_key[192] = {};
    if (!api_key_store_get_speech(speech_key, sizeof(speech_key))) {
        lv_label_set_text(status_label_, "语音密钥未设置");
        return;
    }
    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        lv_label_set_text(status_label_, "Wi-Fi 未连接");
        return;
    }

    releaseKeyboard();
    const audio_player_state_t player_state = audio_player_get_state();
    if ((player_state == AUDIO_PLAYER_STATE_PLAYING) ||
        (player_state == AUDIO_PLAYER_STATE_PAUSE)) {
        audio_player_stop();
    }
    lv_label_set_text(status_label_, "正在聆听，请说话…");
    lv_label_set_text(result_label_, "说完后会自动结束");
    setListeningGlow(true);
    lv_obj_add_state(http_button_, LV_STATE_DISABLED);
    lv_obj_add_state(https_button_, LV_STATE_DISABLED);
    lv_obj_add_state(deepseek_button_, LV_STATE_DISABLED);
    if (speak_button_) lv_obj_add_state(speak_button_, LV_STATE_DISABLED);
    finish_listening_requested_ = false;
    listening_active_ = true;
    setInteractionState(RobotInteractionState::Listening);

    if (xTaskCreate(speechTask, "speech_asr", 12288, this, 4, &request_task_) != pdPASS) {
        request_task_ = nullptr;
        listening_active_ = false;
        lv_label_set_text(status_label_, "录音任务启动失败");
        lv_obj_clear_state(http_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(https_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(deepseek_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(voice_button_, LV_STATE_DISABLED);
        if (speak_button_ && last_answer_[0] != '\0') lv_obj_clear_state(speak_button_, LV_STATE_DISABLED);
        eye_set_state(EYE_SAD);
        setListeningGlow(false);
    }
}

void NetworkTest::startStreamTest()
{
    if (!active_ || request_task_) return;
    char speech_key[192] = {};
    if (!api_key_store_get_speech(speech_key, sizeof(speech_key))) {
        lv_label_set_text(status_label_, "语音密钥未设置");
        return;
    }
    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        lv_label_set_text(status_label_, "Wi-Fi 未连接");
        return;
    }
    releaseKeyboard();
    lv_label_set_text(status_label_, "正在聆听");
    lv_label_set_text(result_label_, "请自然说话，说完后会直接上传识别");
    setListeningGlow(true);
    lv_obj_add_state(http_button_, LV_STATE_DISABLED);
    lv_obj_add_state(https_button_, LV_STATE_DISABLED);
    lv_obj_add_state(deepseek_button_, LV_STATE_DISABLED);
    if (speak_button_) lv_obj_add_state(speak_button_, LV_STATE_DISABLED);
    finish_listening_requested_ = false;
    listening_active_ = true;
    setInteractionState(RobotInteractionState::Listening);
    if (xTaskCreate(speechTask, "direct_asr", 12288, this, 4,
                    &request_task_) != pdPASS) {
        request_task_ = nullptr;
        listening_active_ = false;
        lv_label_set_text(status_label_, "语音识别任务启动失败");
        setListeningGlow(false);
        lv_obj_clear_state(http_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(https_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(deepseek_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(voice_button_, LV_STATE_DISABLED);
    }
}

void NetworkTest::finishListening()
{
    if (!active_ || !listening_active_ || !request_task_) return;
    finish_listening_requested_ = true;
    if (status_label_) lv_label_set_text(status_label_, "正在结束聆听…");
    if (result_label_) lv_label_set_text(result_label_, "正在识别已录音内容");
}

void NetworkTest::streamTestTask(void *arg)
{
    auto *app = static_cast<NetworkTest *>(arg);
    auto *result = static_cast<UiResult *>(calloc(1, sizeof(UiResult)));
    if (result) {
        result->app = app;
        // Reuse the proven ASR -> DeepSeek -> TTS UI pipeline. A successful
        // streaming transcription behaves exactly like the legacy HTTP ASR.
        result->speech = true;
    }

    struct TestState {
        volatile bool acknowledged = false;
        volatile bool server_hello = false;
        volatile bool asr_finished = false;
        bool asr_success = false;
        char recognized_text[256] = {};
        char asr_error[128] = {};
    } state;

    ESP_LOGI(TAG, "Stream test heap before start: internal=%u largest=%u psram=%u",
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)),
             static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_SPIRAM)));

    vela::streaming::StreamProtocol protocol;
    protocol.setEventCallback(
        [](const char *json, size_t length, void *context) {
            auto *test = static_cast<TestState *>(context);
            if (!json || !test) return;
            auto contains = [json, length](const char *token) {
                const size_t token_length = strlen(token);
                if (token_length > length) return false;
                for (size_t offset = 0; offset + token_length <= length; ++offset) {
                    if (memcmp(json + offset, token, token_length) == 0) return true;
                }
                return false;
            };
            // WebSocket event payloads are length-delimited and are not
            // guaranteed to contain a trailing NUL. Never use strstr here.
            if (contains("\"hello\"")) test->server_hello = true;
            if (contains("opus_stream_ok")) test->acknowledged = true;
            cJSON *message = cJSON_ParseWithLength(json, length);
            if (message) {
                const char *type = jsonText(message, "type", "");
                if (strcmp(type, "asr_result") == 0) {
                    snprintf(test->recognized_text, sizeof(test->recognized_text),
                             "%s", jsonText(message, "text", ""));
                    test->asr_success = test->recognized_text[0] != '\0';
                    test->asr_finished = true;
                } else if (strcmp(type, "asr_error") == 0) {
                    snprintf(test->asr_error, sizeof(test->asr_error), "%s",
                             jsonText(message, "error", "ASR failed"));
                    test->asr_finished = true;
                }
                cJSON_Delete(message);
            }
        },
        &state);

    esp_err_t error = protocol.start("ws://192.168.0.101:8000/xiaozhi/v1/");
    const TickType_t connect_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(8000);
    while ((error == ESP_OK) && !protocol.connected() &&
           (xTaskGetTickCount() < connect_deadline)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if ((error == ESP_OK) && !protocol.connected()) error = ESP_ERR_TIMEOUT;

    vela::streaming::StreamOpusEncoder encoder;
    if (error == ESP_OK) {
        error = encoder.init();
        ESP_LOGI(TAG, "Opus init result=%s internal=%u largest=%u",
                 esp_err_to_name(error),
                 static_cast<unsigned>(heap_caps_get_free_size(MALLOC_CAP_INTERNAL)),
                 static_cast<unsigned>(heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL)));
    }
    if (error == ESP_OK) error = protocol.startListening();

    const size_t pcm_bytes = encoder.pcmBytesPerFrame();
    const size_t mono_samples = pcm_bytes / sizeof(int16_t);
    int16_t *mono = static_cast<int16_t *>(heap_caps_malloc(
        pcm_bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *stereo = static_cast<uint8_t *>(heap_caps_malloc(
        pcm_bytes * 2, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    uint8_t *opus = static_cast<uint8_t *>(heap_caps_malloc(
        encoder.maxOpusBytes(), MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
    if ((error == ESP_OK) && (!mono || !stereo || !opus)) error = ESP_ERR_NO_MEM;

    constexpr size_t VAD_SPEECH_CONFIRM_FRAMES = 3;   // 180 ms
    constexpr size_t VAD_END_SILENCE_FRAMES = 12;    // 720 ms
    constexpr size_t VAD_NO_SPEECH_TIMEOUT = 80;     // 4.8 s
    constexpr size_t VAD_MAX_FRAMES = 200;            // 12 s safety limit
    uint32_t noise_level = 120;
    size_t sent_frames = 0;
    size_t voiced_run = 0;
    size_t silence_run = 0;
    bool speech_started = false;
    while ((error == ESP_OK) && !app->finish_listening_requested_ &&
           sent_frames < VAD_MAX_FRAMES) {
        size_t bytes_read = 0;
        error = bsp_extra_i2s_read(stereo, pcm_bytes * 2, &bytes_read, 1000);
        if ((error != ESP_OK) || bytes_read < pcm_bytes * 2) {
            error = ESP_FAIL;
            break;
        }
        const int16_t *stereo_samples = reinterpret_cast<const int16_t *>(stereo);
        for (size_t i = 0; i < mono_samples; ++i) mono[i] = stereo_samples[i * 2];

        uint64_t absolute_sum = 0;
        for (size_t i = 0; i < mono_samples; ++i) {
            const int32_t sample = mono[i];
            absolute_sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
        }
        const uint32_t level = static_cast<uint32_t>(absolute_sum / mono_samples);
        const uint32_t speech_threshold =
            (noise_level * 5U / 2U > VAD_MIN_LEVEL) ? noise_level * 5U / 2U : VAD_MIN_LEVEL;
        const bool voiced = level >= speech_threshold;
        if (!speech_started && !voiced) {
            noise_level = (noise_level * 7U + level) / 8U;
            if (noise_level > 200) noise_level = 200;
        }
        if (voiced) {
            ++voiced_run;
            silence_run = 0;
            if (voiced_run >= VAD_SPEECH_CONFIRM_FRAMES) speech_started = true;
        } else {
            voiced_run = 0;
            if (speech_started) ++silence_run;
        }
        size_t opus_length = 0;
        error = encoder.encode(mono, mono_samples, opus, encoder.maxOpusBytes(),
                               &opus_length);
        if (error == ESP_OK) error = protocol.sendOpus(opus, opus_length);
        if (error == ESP_OK) ++sent_frames;
        if (speech_started && silence_run >= VAD_END_SILENCE_FRAMES) break;
        if (!speech_started && sent_frames >= VAD_NO_SPEECH_TIMEOUT) break;
    }
    ESP_LOGI(TAG,
             "VAD finished: frames=%u speech=%d silence=%u noise=%u",
             static_cast<unsigned>(sent_frames), speech_started,
             static_cast<unsigned>(silence_run), static_cast<unsigned>(noise_level));
    const bool manually_finished = app->finish_listening_requested_;
    if ((error == ESP_OK) && !manually_finished && !speech_started) error = ESP_ERR_NOT_FOUND;
    if (protocol.connected()) protocol.stopListening();

    const TickType_t ack_deadline = xTaskGetTickCount() + pdMS_TO_TICKS(35000);
    while ((error == ESP_OK) && !state.acknowledged &&
           (xTaskGetTickCount() < ack_deadline)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }
    if ((error == ESP_OK) && !state.acknowledged) error = ESP_ERR_TIMEOUT;
    while ((error == ESP_OK) && !state.asr_finished &&
           (xTaskGetTickCount() < ack_deadline)) {
        vTaskDelay(pdMS_TO_TICKS(50));
    }

    protocol.stop();
    encoder.deinit();
    if (mono) heap_caps_free(mono);
    if (stereo) heap_caps_free(stereo);
    if (opus) heap_caps_free(opus);

    if (result) {
        result->cancelled = false;
        result->success = (error == ESP_OK) && state.asr_success;
        if (result->success) {
            snprintf(result->title, sizeof(result->title), "流式语音识别成功");
            snprintf(result->body, sizeof(result->body), "%s", state.recognized_text);
        } else if (state.asr_error[0]) {
            snprintf(result->title, sizeof(result->title), "流式语音识别失败");
            snprintf(result->body, sizeof(result->body), "%s", state.asr_error);
        } else {
            snprintf(result->title, sizeof(result->title), "流式语音识别失败");
            snprintf(result->body, sizeof(result->body),
                     "Error: %s；请确认电脑 ASR 服务", esp_err_to_name(error));
        }
    }
    app->listening_active_ = false;
    app->finish_listening_requested_ = false;
    app->request_task_ = nullptr;
    if (result) lv_async_call(applyResultAsync, result);
    vTaskDelete(nullptr);
}

void NetworkTest::startTts()
{
    if (!active_ || request_task_ || last_answer_[0] == '\0') return;
    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        lv_label_set_text(status_label_, "Wi-Fi 未连接");
        return;
    }
    lv_label_set_text(status_label_, "正在生成语音…");
    lv_label_set_text(result_label_, last_answer_);
    lv_obj_add_state(http_button_, LV_STATE_DISABLED);
    lv_obj_add_state(https_button_, LV_STATE_DISABLED);
    lv_obj_add_state(deepseek_button_, LV_STATE_DISABLED);
    lv_obj_add_state(voice_button_, LV_STATE_DISABLED);
    lv_obj_add_state(speak_button_, LV_STATE_DISABLED);
    setInteractionState(RobotInteractionState::Speaking);
    if (xTaskCreate(ttsTask, "speech_tts", 12288, this, 4, &request_task_) != pdPASS) {
        request_task_ = nullptr;
        lv_label_set_text(status_label_, "语音生成任务失败");
        lv_obj_clear_state(http_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(https_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(deepseek_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(voice_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(speak_button_, LV_STATE_DISABLED);
        eye_set_state(EYE_SAD);
    }
}

void NetworkTest::startRequest(RequestType type)
{
    if (!active_ || request_task_) return;

    if (type == RequestType::DeepSeek) {
        char deepseek_key[192] = {};
        if (!api_key_store_get_deepseek(deepseek_key, sizeof(deepseek_key))) {
            lv_label_set_text(status_label_, "DeepSeek 密钥未设置");
            lv_label_set_text(result_label_, "请先配置 API 密钥");
            return;
        }
        const char *text = prompt_textarea_ ? lv_textarea_get_text(prompt_textarea_) : nullptr;
        if (!text || text[0] == '\0') {
            lv_label_set_text(status_label_, "Question Required");
            lv_label_set_text(result_label_, "Type a question first");
            return;
        }
        snprintf(prompt_, sizeof(prompt_), "%s", text);
        releaseKeyboard();
        setInteractionState(RobotInteractionState::Thinking);
    }

    wifi_ap_record_t access_point = {};
    if (esp_wifi_sta_get_ap_info(&access_point) != ESP_OK) {
        if (type == RequestType::DeepSeek) eye_set_state(EYE_SAD);
        lv_label_set_text(status_label_, "Wi-Fi Offline");
        lv_label_set_text(result_label_, "Connect Wi-Fi in Settings, then try again");
        lv_obj_set_style_text_color(status_label_, lv_color_hex(0xFF9B9B), 0);
        return;
    }

    lv_label_set_text(status_label_, "Requesting...");
    request_type_ = type;
    lv_label_set_text(result_label_, type == RequestType::DeepSeek ? "Sending a message to DeepSeek" :
                                      type == RequestType::Https ? "TLS handshake and certificate check" :
                                                                  "Waiting for HTTP JSON response");
    lv_obj_set_style_text_color(status_label_, lv_color_hex(0x91E8F8), 0);
    lv_obj_add_state(http_button_, LV_STATE_DISABLED);
    lv_obj_add_state(https_button_, LV_STATE_DISABLED);
    lv_obj_add_state(deepseek_button_, LV_STATE_DISABLED);
    lv_obj_add_state(voice_button_, LV_STATE_DISABLED);
    if (speak_button_) lv_obj_add_state(speak_button_, LV_STATE_DISABLED);

    const BaseType_t created = xTaskCreate(
        requestTask, type == RequestType::DeepSeek ? "deepseek" :
                     type == RequestType::Https ? "api_https" : "api_http",
        type == RequestType::Http ? 6144 : 12288, this, 4, &request_task_);
    if (created != pdPASS) {
        if (type == RequestType::DeepSeek) eye_set_state(EYE_SAD);
        request_task_ = nullptr;
        lv_label_set_text(status_label_, "Start Failed");
        lv_label_set_text(result_label_, "Not enough memory for HTTP task");
        lv_obj_clear_state(http_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(https_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(deepseek_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(voice_button_, LV_STATE_DISABLED);
        if (speak_button_ && last_answer_[0] != '\0') lv_obj_clear_state(speak_button_, LV_STATE_DISABLED);
    }
}

void NetworkTest::applyResultAsync(void *arg)
{
    auto *result = static_cast<UiResult *>(arg);
    if (!result) return;
    NetworkTest *app = result->app;
    if (app && app->active_ && app->status_label_ && app->result_label_ &&
        app->http_button_ && app->https_button_ && app->deepseek_button_ &&
         app->voice_button_ && app->speak_button_) {
        lv_label_set_text(app->status_label_, result->title);
        lv_label_set_text(app->result_label_, result->body);
        lv_obj_set_style_text_color(
            app->status_label_,
            result->success ? lv_color_hex(0x91E8F8) : lv_color_hex(0xFF9B9B), 0);
        lv_obj_clear_state(app->http_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(app->https_button_, LV_STATE_DISABLED);
        lv_obj_clear_state(app->deepseek_button_, LV_STATE_DISABLED);
         lv_obj_clear_state(app->voice_button_, LV_STATE_DISABLED);
         app->listening_active_ = false;
        if (app->last_answer_[0] != '\0') {
            lv_obj_clear_state(app->speak_button_, LV_STATE_DISABLED);
        }
        if (result->speech && result->success && app->prompt_textarea_) {
            app->consecutive_listen_failures_ = 0;
            char query[sizeof(app->prompt_)] = {};
            const TranscriptAction action =
                app->interaction_.handleTranscript(result->body, query, sizeof(query));
            if (action == TranscriptAction::Ignored) {
                lv_label_set_text(app->status_label_, "没有听清，请再说一次");
                lv_label_set_text(app->result_label_, "可以直接说话");
                app->setInteractionState(RobotInteractionState::Listening);
                app->scheduleAutoListen(350);
            } else {
                snprintf(app->prompt_, sizeof(app->prompt_), "%s", query);
                lv_textarea_set_text(app->prompt_textarea_, app->prompt_);
                lv_label_set_text(app->status_label_, "正在思考");
                lv_label_set_text(app->result_label_, "正在准备回答…");
                app->setInteractionState(RobotInteractionState::Thinking);
                lv_timer_t *timer = lv_timer_create(delayedDeepSeek, 250, app);
                if (timer) lv_timer_set_repeat_count(timer, 1);
            }
        }
        if (result->speech && !result->success && !result->cancelled) {
            if (app->consecutive_listen_failures_ < 10) {
                ++app->consecutive_listen_failures_;
            }
            app->setInteractionState(RobotInteractionState::Error);
            const uint32_t retry_delay = app->consecutive_listen_failures_ >= 3 ?
                                         5000 : 1500;
            app->scheduleAutoListen(retry_delay);
        }
        if (result->deepseek) {
            if (result->success) {
                size_t history_index = app->history_count_;
                if (history_index >= app->CONVERSATION_HISTORY_LIMIT) {
                    memmove(app->history_user_[0], app->history_user_[1],
                            sizeof(app->history_user_[0]) *
                                (app->CONVERSATION_HISTORY_LIMIT - 1));
                    memmove(app->history_assistant_[0], app->history_assistant_[1],
                            sizeof(app->history_assistant_[0]) *
                                (app->CONVERSATION_HISTORY_LIMIT - 1));
                    history_index = app->CONVERSATION_HISTORY_LIMIT - 1;
                } else {
                    ++app->history_count_;
                }
                snprintf(app->history_user_[history_index],
                         sizeof(app->history_user_[history_index]), "%s", app->prompt_);
                snprintf(app->history_assistant_[history_index],
                         sizeof(app->history_assistant_[history_index]), "%s",
                         result->body);
                snprintf(app->last_answer_, sizeof(app->last_answer_), "%s", result->body);
                lv_obj_clear_state(app->speak_button_, LV_STATE_DISABLED);
                lv_label_set_text(app->status_label_, "回答完成，正在准备朗读…");
                // Give the C6/TLS stack time to release the DeepSeek connection
                // before opening the SiliconFlow TTS connection.
                lv_timer_t *timer = lv_timer_create(delayedTts, 180, app);
                if (timer) lv_timer_set_repeat_count(timer, 1);
            } else {
                app->scheduleAutoListen(1500);
            }
            app->setInteractionState(result->success ? RobotInteractionState::Speaking :
                                                       RobotInteractionState::Error);
        }
        if (result->speech) {
            app->setListeningGlow(false);
        }
        if (result->tts) {
            if (result->success) {
                app->setInteractionState(app->interaction_.sessionActive() ?
                    RobotInteractionState::Listening : RobotInteractionState::Listening);
                lv_label_set_text(app->status_label_, "可以继续说");
            } else {
                app->setInteractionState(RobotInteractionState::Error);
            }
            app->scheduleAutoListen(result->success ? 650 : 1500);
        }
    }
    free(result);
}

void NetworkTest::speechTask(void *arg)
{
    auto *app = static_cast<NetworkTest *>(arg);
    char speech_key[192] = {};
    const bool has_speech_key = api_key_store_get_speech(speech_key, sizeof(speech_key));
    auto *result = static_cast<UiResult *>(calloc(1, sizeof(UiResult)));
    if (result) {
        result->app = app;
        result->speech = true;
    }

    static const char boundary[] = "----VelaSpeechBoundary7MA4YWxk";
    char prefix[384] = {};
    const int prefix_len = snprintf(
        prefix, sizeof(prefix),
        "--%s\r\nContent-Disposition: form-data; name=\"model\"\r\n\r\n"
        "FunAudioLLM/SenseVoiceSmall\r\n"
        "--%s\r\nContent-Disposition: form-data; name=\"file\"; filename=\"speech.wav\"\r\n"
        "Content-Type: audio/wav\r\n\r\n",
        boundary, boundary);
    char footer[96] = {};
    const int footer_len = snprintf(footer, sizeof(footer), "\r\n--%s--\r\n", boundary);
    const size_t maximum_body_size = prefix_len + 44 + SPEECH_MAX_PCM_BYTES + footer_len;
    size_t body_size = 0;
    uint8_t *body = static_cast<uint8_t *>(
        heap_caps_malloc(maximum_body_size, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));

    esp_err_t error = !has_speech_key ? ESP_ERR_INVALID_STATE :
                      (body ? ESP_OK : ESP_ERR_NO_MEM);
    if (body) {
        memcpy(body, prefix, prefix_len);
        uint8_t *wav = body + prefix_len;
        uint8_t *pcm = wav + 44;
        size_t captured = 0;
        constexpr size_t FRAME_MS = 60;
        constexpr size_t MONO_FRAME_BYTES = SPEECH_SAMPLE_RATE * (SPEECH_BITS / 8) *
                                            FRAME_MS / 1000;
        constexpr size_t STEREO_FRAME_BYTES = MONO_FRAME_BYTES * SPEECH_CAPTURE_CHANNELS;
        constexpr size_t SPEECH_CONFIRM_FRAMES = 3;
        constexpr size_t END_SILENCE_FRAMES = 12;
        constexpr size_t NO_SPEECH_FRAMES = 80;
        constexpr size_t PRE_ROLL_FRAMES = 5;
        uint8_t stereo_chunk[STEREO_FRAME_BYTES] = {};
        uint32_t noise_level = 120;
        size_t frame_count = 0;
        size_t voiced_run = 0;
        size_t silence_run = 0;
        size_t speech_start_offset = 0;
        bool speech_started = false;
        while ((captured < SPEECH_MAX_PCM_BYTES) && (error == ESP_OK) &&
               !app->finish_listening_requested_) {
            size_t bytes_read = 0;
            error = bsp_extra_i2s_read(stereo_chunk, sizeof(stereo_chunk), &bytes_read, 1000);
            if ((error != ESP_OK) || (bytes_read < sizeof(stereo_chunk))) break;
            const size_t frames = bytes_read / 4;
            uint64_t absolute_sum = 0;
            for (size_t frame = 0; frame < frames; ++frame) {
                const int16_t sample = static_cast<int16_t>(
                    static_cast<uint16_t>(stereo_chunk[frame * 4]) |
                    (static_cast<uint16_t>(stereo_chunk[frame * 4 + 1]) << 8));
                pcm[captured++] = stereo_chunk[frame * 4];
                pcm[captured++] = stereo_chunk[frame * 4 + 1];
                absolute_sum += static_cast<uint32_t>(sample < 0 ? -sample : sample);
            }
            ++frame_count;
            const uint32_t level = static_cast<uint32_t>(absolute_sum / frames);
            const uint32_t threshold = noise_level * 5U / 2U > VAD_MIN_LEVEL ? noise_level * 5U / 2U : VAD_MIN_LEVEL;
            const bool voiced = level >= threshold;
            if (!speech_started && !voiced) {
                noise_level = (noise_level * 7U + level) / 8U;
                if (noise_level > 200) noise_level = 200;
            }
            if (voiced) {
                ++voiced_run;
                silence_run = 0;
                if (!speech_started && voiced_run >= SPEECH_CONFIRM_FRAMES) {
                    speech_started = true;
                    const size_t pre_roll = PRE_ROLL_FRAMES * MONO_FRAME_BYTES;
                    speech_start_offset = captured > pre_roll ? captured - pre_roll : 0;
                }
            } else {
                voiced_run = 0;
                if (speech_started) ++silence_run;
            }
            if (speech_started && silence_run >= END_SILENCE_FRAMES) break;
            if (!speech_started && frame_count >= NO_SPEECH_FRAMES) break;
        }
        const bool manually_finished = app->finish_listening_requested_;
        if ((error == ESP_OK) && !manually_finished && !speech_started) error = ESP_ERR_NOT_FOUND;
        if ((error == ESP_OK) && speech_start_offset > 0) {
            memmove(pcm, pcm + speech_start_offset, captured - speech_start_offset);
            captured -= speech_start_offset;
        }
        if (error == ESP_OK) {
            makeWavHeader(wav, captured);
            body_size = prefix_len + 44 + captured + footer_len;
            memcpy(body + prefix_len + 44 + captured, footer, footer_len);
            ESP_LOGI(TAG, "Direct ASR capture: %u ms, bytes=%u",
                     static_cast<unsigned>(captured * 1000 /
                         (SPEECH_SAMPLE_RATE * (SPEECH_BITS / 8))),
                     static_cast<unsigned>(captured));
        }
    }

    int status_code = 0;
    esp_http_client_handle_t client = nullptr;
    if (error == ESP_OK) {
        esp_wifi_scan_stop();
        vTaskDelay(pdMS_TO_TICKS(180));
        static const uint32_t s_asr_timeouts[] = {3000, 5000, 7000, 9000, 12000, 15000};
        const int max_asr_attempts = static_cast<int>(sizeof(s_asr_timeouts) / sizeof(s_asr_timeouts[0]));
        for (int attempt = 0; attempt < max_asr_attempts; ++attempt) {
            memset(&s_speech_buffer, 0, sizeof(s_speech_buffer));
            esp_http_client_config_t config = {};
            config.url = SPEECH_URL;
            config.event_handler = httpClientEvent;
            config.user_data = &s_speech_buffer;
            config.timeout_ms = s_asr_timeouts[attempt];
            config.crt_bundle_attach = esp_crt_bundle_attach;
            config.keep_alive_enable = false;
            client = esp_http_client_init(&config);
            if (!client) {
                error = ESP_ERR_NO_MEM;
            } else {
                char authorization[192] = {};
                char content_type[128] = {};
                snprintf(authorization, sizeof(authorization), "Bearer %s", speech_key);
                snprintf(content_type, sizeof(content_type),
                         "multipart/form-data; boundary=%s", boundary);
                esp_http_client_set_method(client, HTTP_METHOD_POST);
                esp_http_client_set_header(client, "Authorization", authorization);
                esp_http_client_set_header(client, "Content-Type", content_type);
                esp_http_client_set_post_field(client,
                    reinterpret_cast<const char *>(body), body_size);
                error = esp_http_client_perform(client);
                status_code = esp_http_client_get_status_code(client);
                ESP_LOGI(TAG, "Speech API attempt=%d status=%d response=%s",
                         attempt + 1, status_code, s_speech_buffer.data);
                esp_http_client_cleanup(client);
                client = nullptr;
            }
            if ((error == ESP_OK) && (status_code == 200)) break;
            if (client) esp_http_client_close(client);
            esp_http_client_cleanup(client);
            client = nullptr;
            if (attempt < max_asr_attempts - 1) {
                error = ESP_OK;
                vTaskDelay(pdMS_TO_TICKS(400));
            }
        }
    }

    bool success = false;
    if (result) {
        result->cancelled = false;
        if ((error == ESP_OK) && (status_code == 200) && !s_speech_buffer.overflow) {
            cJSON *json = cJSON_Parse(s_speech_buffer.data);
            const char *text = json ? jsonText(json, "text", "") : "";
            if (text[0] != '\0') {
                snprintf(result->title, sizeof(result->title), "语音识别成功");
                snprintf(result->body, sizeof(result->body), "%s", text);
                success = true;
            } else {
                snprintf(result->title, sizeof(result->title), "没有识别到语音");
                snprintf(result->body, sizeof(result->body), "请靠近麦克风后重新录音");
            }
            if (json) cJSON_Delete(json);
        } else {
            snprintf(result->title, sizeof(result->title), "语音识别失败");
            if (s_speech_buffer.data[0] != '\0') {
                snprintf(result->body, sizeof(result->body),
                         "Status: %d\n%s", status_code, s_speech_buffer.data);
            } else {
                snprintf(result->body, sizeof(result->body),
                         "Status: %d\nError: %s", status_code, esp_err_to_name(error));
            }
        }
        result->success = success;
    }

    if (client) esp_http_client_cleanup(client);
    if (body) heap_caps_free(body);
    app->listening_active_ = false;
    app->finish_listening_requested_ = false;
    app->request_task_ = nullptr;
    if (result) lv_async_call(applyResultAsync, result);
    vTaskDelete(nullptr);
}

void NetworkTest::ttsTask(void *arg)
{
    auto *app = static_cast<NetworkTest *>(arg);
    char speech_key[192] = {};
    const bool has_speech_key = api_key_store_get_speech(speech_key, sizeof(speech_key));
    const int64_t tts_started_us = esp_timer_get_time();
    auto *result = static_cast<UiResult *>(calloc(1, sizeof(UiResult)));
    if (result) {
        result->app = app;
        result->tts = true;
    }

    esp_wifi_scan_stop();
    vTaskDelay(pdMS_TO_TICKS(40));

    // A previous MP3 may still own the SPIFFS file when the next question is
    // answered. Stop it before truncating/replacing the shared TTS file.
    if (bsp_extra_player_init() == ESP_OK) {
        const audio_player_state_t player_state = audio_player_get_state();
        if ((player_state == AUDIO_PLAYER_STATE_PLAYING) ||
            (player_state == AUDIO_PLAYER_STATE_PAUSE)) {
            audio_player_stop();
            vTaskDelay(pdMS_TO_TICKS(60));
        }
    }

    char fast_tts_text[256] = {};
    makeFastTtsText(app->last_answer_, fast_tts_text, sizeof(fast_tts_text));
    ESP_LOGI(TAG, "TTS fast segment bytes=%u full_answer_bytes=%u",
             static_cast<unsigned>(strlen(fast_tts_text)),
             static_cast<unsigned>(strlen(app->last_answer_)));

    cJSON *request_json = cJSON_CreateObject();
    char *post_body = nullptr;
    if (request_json) {
        cJSON_AddStringToObject(request_json, "model", "FunAudioLLM/CosyVoice2-0.5B");
        cJSON_AddStringToObject(request_json, "input", fast_tts_text);
        cJSON_AddStringToObject(request_json, "voice", "FunAudioLLM/CosyVoice2-0.5B:anna");
        cJSON_AddStringToObject(request_json, "response_format", "mp3");
        cJSON_AddNumberToObject(request_json, "speed", 1.18);
        post_body = cJSON_PrintUnformatted(request_json);
        cJSON_Delete(request_json);
    }

    TtsDownload download = {};
    esp_err_t error = !has_speech_key ? ESP_ERR_INVALID_STATE :
                      (post_body ? ESP_OK : ESP_ERR_NO_MEM);
    int status_code = 0;
    static const uint32_t tts_timeouts[] = {3000, 5000, 7000, 9000};
    const int max_tts_attempts = static_cast<int>(sizeof(tts_timeouts) / sizeof(tts_timeouts[0]));
    for (int attempt = 0; (attempt < max_tts_attempts) && (error == ESP_OK); ++attempt) {
        memset(&download, 0, sizeof(download));
        download.file = fopen(TTS_FILE, "wb");
        if (!download.file) {
            error = ESP_FAIL;
            break;
        }
        download.last_data_us = esp_timer_get_time();
        download.started_us = download.last_data_us;
        esp_http_client_config_t config = {};
        config.url = TTS_URL;
        config.event_handler = ttsClientEvent;
        config.user_data = &download;
        config.timeout_ms = tts_timeouts[attempt];
        config.crt_bundle_attach = esp_crt_bundle_attach;
        config.keep_alive_enable = false;
        esp_http_client_handle_t client = esp_http_client_init(&config);
        if (!client) {
            fclose(download.file);
            download.file = nullptr;
            error = ESP_ERR_NO_MEM;
            break;
        }
        char authorization[192] = {};
        snprintf(authorization, sizeof(authorization), "Bearer %s", speech_key);
        esp_http_client_set_method(client, HTTP_METHOD_POST);
        esp_http_client_set_header(client, "Authorization", authorization);
        esp_http_client_set_header(client, "Content-Type", "application/json");
        esp_http_client_set_post_field(client, post_body, strlen(post_body));
        error = esp_http_client_perform(client);
        status_code = esp_http_client_get_status_code(client);
        ESP_LOGI(TAG, "TTS HTTP attempt=%d elapsed=%lld ms status=%d bytes=%u",
                 attempt + 1,
                 static_cast<long long>((esp_timer_get_time() - tts_started_us) / 1000),
                 status_code, static_cast<unsigned>(download.length));
        fclose(download.file);
        download.file = nullptr;
        esp_http_client_cleanup(client);
        if ((error == ESP_OK) && (status_code == 200) && !download.failed &&
            (download.length > 128)) {
            break;
        }
        ESP_LOGW(TAG, "TTS attempt failed, retrying: status=%d error=%s bytes=%u",
                 status_code, esp_err_to_name(error),
                 static_cast<unsigned>(download.length));
        error = ESP_OK;
        vTaskDelay(pdMS_TO_TICKS(400));
    }
    if (post_body) free(post_body);

    bool success = false;
    if ((error == ESP_OK) && (status_code == 200) && !download.failed &&
        (download.length > 128)) {
        ESP_LOGI(TAG, "TTS ready to play after %lld ms",
                 static_cast<long long>((esp_timer_get_time() - tts_started_us) / 1000));
        int applied_volume = 0;
        bsp_extra_codec_volume_set(100, &applied_volume);
        if (bsp_extra_player_play_file(TTS_FILE) == ESP_OK) {
            success = true;
            // Do not reopen the microphone until the speaker has completely
            // finished. This prevents the robot from recognizing its own TTS.
            vTaskDelay(pdMS_TO_TICKS(120));
            for (int elapsed_ms = 0; elapsed_ms < 20000; elapsed_ms += 100) {
                const audio_player_state_t state = audio_player_get_state();
                if ((state != AUDIO_PLAYER_STATE_PLAYING) &&
                    (state != AUDIO_PLAYER_STATE_PAUSE)) {
                    break;
                }
                vTaskDelay(pdMS_TO_TICKS(100));
            }
            // FINISHED/IDLE doesn't guarantee that the decoder and shared
            // audio codec resources have been released. Explicitly stop the
            // player before reopening the Opus encoder for the next turn.
            audio_player_stop();
            vTaskDelay(pdMS_TO_TICKS(350));
        }
    }

    if (result) {
        result->success = success;
        if (success) {
            snprintf(result->title, sizeof(result->title), "正在朗读");
            snprintf(result->body, sizeof(result->body), "%s", app->last_answer_);
        } else {
            snprintf(result->title, sizeof(result->title), "语音生成失败");
            snprintf(result->body, sizeof(result->body),
                     "Status: %d\nError: %s\nBytes: %u",
                     status_code, esp_err_to_name(error),
                     static_cast<unsigned>(download.length));
        }
    }

    app->request_task_ = nullptr;
    if (result) lv_async_call(applyResultAsync, result);
    vTaskDelete(nullptr);
}

void NetworkTest::requestTask(void *arg)
{
    auto *app = static_cast<NetworkTest *>(arg);
    const RequestType type = app->request_type_;
    const bool use_https = type != RequestType::Http;
    const bool use_deepseek = type == RequestType::DeepSeek;
    HttpBuffer *buffer = use_deepseek ? &s_deepseek_buffer :
                         use_https ? &s_https_buffer : &s_http_buffer;
    memset(buffer, 0, sizeof(*buffer));
    auto *result = static_cast<UiResult *>(calloc(1, sizeof(UiResult)));

    if (use_https) {
        // The P4 reaches Wi-Fi through the C6 over SDIO. Make sure a scan left
        // by Settings has fully stopped before the TLS handshake starts.
        const esp_err_t scan_stop_result = esp_wifi_scan_stop();
        if ((scan_stop_result != ESP_OK) &&
            (scan_stop_result != ESP_ERR_WIFI_STATE) &&
            (scan_stop_result != ESP_ERR_WIFI_NOT_STARTED)) {
            ESP_LOGW(TAG, "esp_wifi_scan_stop before HTTPS failed: %s",
                     esp_err_to_name(scan_stop_result));
        }
        vTaskDelay(pdMS_TO_TICKS(100));
    }

    if (use_deepseek) {
        // Avoid proxy/Fake-IP DNS answers which work on a PC proxy but are
        // unreachable from the ESP32. Prefer public DNS servers reachable
        // directly from networks in China.
        ip_addr_t primary_dns = {};
        ip_addr_t secondary_dns = {};
        if (ipaddr_aton("223.5.5.5", &primary_dns)) dns_setserver(0, &primary_dns);
        if (ipaddr_aton("119.29.29.29", &secondary_dns)) dns_setserver(1, &secondary_dns);
    }

    esp_http_client_config_t config = {};
    config.url = use_deepseek ? DEEPSEEK_URL : use_https ? HTTPS_URL : API_URL;
    config.event_handler = httpClientEvent;
    config.user_data = buffer;
    config.timeout_ms = use_deepseek ? 20000 : 6000;
    config.keep_alive_enable = true;
    if (use_https) config.crt_bundle_attach = esp_crt_bundle_attach;

    esp_http_client_handle_t client = nullptr;
    char authorization[192] = {};
    char *post_body = nullptr;

    if (use_deepseek) {
        cJSON *request_json = cJSON_CreateObject();
        cJSON *messages = cJSON_CreateArray();
        cJSON *system_message = cJSON_CreateObject();
        if (request_json && messages && system_message) {
            cJSON_AddStringToObject(request_json, "model", "deepseek-chat");
            cJSON_AddStringToObject(system_message, "role", "system");
            cJSON_AddStringToObject(system_message, "content",
                                    "请使用简体中文直接回答，语言自然、准确。"
                                    "结合最近对话理解用户的省略、代词和追问。");
            cJSON_AddItemToArray(messages, system_message);
            system_message = nullptr;
            auto add_message = [messages](const char *role, const char *content) {
                cJSON *message = cJSON_CreateObject();
                if (!message) return false;
                cJSON_AddStringToObject(message, "role", role);
                cJSON_AddStringToObject(message, "content", content);
                cJSON_AddItemToArray(messages, message);
                return true;
            };
            for (size_t i = 0; i < app->history_count_; ++i) {
                if (!add_message("user", app->history_user_[i]) ||
                    !add_message("assistant", app->history_assistant_[i])) {
                    break;
                }
            }
            add_message("user", app->prompt_);
            cJSON_AddItemToObject(request_json, "messages", messages);
            messages = nullptr;
            cJSON_AddFalseToObject(request_json, "stream");
            cJSON_AddNumberToObject(request_json, "max_tokens", 2048);
            post_body = cJSON_PrintUnformatted(request_json);
        }
        if (system_message) cJSON_Delete(system_message);
        if (messages) cJSON_Delete(messages);
        if (request_json) cJSON_Delete(request_json);
    }

    if (use_deepseek) {
        char deepseek_key[192] = {};
        if (api_key_store_get_deepseek(deepseek_key, sizeof(deepseek_key))) {
            if (!s_deepseek_client) {
                s_deepseek_client = esp_http_client_init(&config);
                if (s_deepseek_client) {
                    snprintf(authorization, sizeof(authorization), "Bearer %s", deepseek_key);
                    esp_http_client_set_method(s_deepseek_client, HTTP_METHOD_POST);
                    esp_http_client_set_header(s_deepseek_client, "Content-Type", "application/json");
                    esp_http_client_set_header(s_deepseek_client, "Authorization", authorization);
                    esp_http_client_set_header(s_deepseek_client, "Connection", "keep-alive");
                }
            }
            client = s_deepseek_client;
            if (client && post_body) {
                esp_http_client_set_post_field(client, post_body, strlen(post_body));
            }
        }
    } else if (use_https) {
        if (!s_https_client) {
            s_https_client = esp_http_client_init(&config);
            if (s_https_client) {
                esp_http_client_set_header(s_https_client, "Connection", "keep-alive");
            }
        }
        client = s_https_client;
    } else {
        client = esp_http_client_init(&config);
    }
    const esp_err_t error = client ? esp_http_client_perform(client) : ESP_ERR_NO_MEM;
    const int status_code = client ? esp_http_client_get_status_code(client) : 0;

    bool success = false;
    if (result) {
        result->app = app;
        result->deepseek = use_deepseek;
        if ((error == ESP_OK) && (status_code == 200) && !buffer->overflow) {
            cJSON *json = cJSON_Parse(buffer->data);
            const char *status = json ? jsonText(json, "status", "") : "";
            if (use_deepseek && json) {
                cJSON *choices = cJSON_GetObjectItemCaseSensitive(json, "choices");
                cJSON *choice = cJSON_IsArray(choices) ? cJSON_GetArrayItem(choices, 0) : nullptr;
                cJSON *message = cJSON_IsObject(choice) ?
                    cJSON_GetObjectItemCaseSensitive(choice, "message") : nullptr;
                const char *content = cJSON_IsObject(message) ?
                    jsonText(message, "content", "No answer returned") : "No answer returned";
                snprintf(result->title, sizeof(result->title), "DeepSeek Connected");
                snprintf(result->body, sizeof(result->body), "%s", content);
                success = cJSON_IsObject(message);
            } else if (use_https && json) {
                cJSON *slideshow = cJSON_GetObjectItemCaseSensitive(json, "slideshow");
                const char *title = cJSON_IsObject(slideshow) ?
                    jsonText(slideshow, "title", "JSON received") : "JSON received";
                snprintf(result->title, sizeof(result->title), "HTTPS Connected");
                snprintf(result->body, sizeof(result->body),
                         "TLS certificate verified\nJSON parsed successfully\n\nServer: httpbin.org\nTitle: %s",
                         title);
                success = true;
            } else if (json && strcmp(status, "success") == 0) {
                snprintf(result->title, sizeof(result->title), "API Connected");
                snprintf(result->body, sizeof(result->body),
                         "JSON parsed successfully\n\nCountry: %s\nRegion: %s\nCity: %s\nISP: %s",
                         jsonText(json, "country", "Unknown"),
                         jsonText(json, "regionName", "Unknown"),
                         jsonText(json, "city", "Unknown"),
                         jsonText(json, "isp", "Unknown"));
                success = true;
            } else {
                snprintf(result->title, sizeof(result->title), "JSON Failed");
                snprintf(result->body, sizeof(result->body),
                         "Server response was not valid JSON\nHTTP status: %d", status_code);
            }
            if (json) cJSON_Delete(json);
        } else {
            snprintf(result->title, sizeof(result->title), "API Failed");
            snprintf(result->body, sizeof(result->body),
                     "%s request failed\nStatus: %d\nError: %s\n\nCheck Wi-Fi and try again",
                     use_deepseek ? "DeepSeek" : use_https ? "HTTPS" : "HTTP",
                     status_code, esp_err_to_name(error));
        }
        result->success = success;
    }

    // Speech recognition and DeepSeek use different TLS servers. Keeping the
    // DeepSeek socket alive after a reply leaves TLS/socket resources occupied
    // and can make the next speech request fail. Release it after every turn.
    if (use_deepseek && s_deepseek_client) {
        esp_http_client_cleanup(s_deepseek_client);
        s_deepseek_client = nullptr;
        client = nullptr;
    } else if (error != ESP_OK) {
        if (use_https && s_https_client) {
            esp_http_client_cleanup(s_https_client);
            s_https_client = nullptr;
        }
    }
    if (client && !use_https) esp_http_client_cleanup(client);
    if (post_body) free(post_body);
    app->request_task_ = nullptr;
    if (result) lv_async_call(applyResultAsync, result);
    else ESP_LOGE(TAG, "Unable to allocate UI result");
    vTaskDelete(nullptr);
}
