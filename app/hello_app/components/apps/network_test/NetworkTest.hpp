#pragma once

#include "esp_brookesia.hpp"
#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "lvgl.h"
#include "interaction/RobotInteractionState.hpp"

class NetworkTest: public ESP_Brookesia_PhoneApp
{
public:
    NetworkTest();
    ~NetworkTest();
    bool init(void) override;
    bool run(void);
    bool back(void);
    bool close(void);

private:
    enum class RequestType {
        Http,
        Https,
        DeepSeek,
    };

    struct UiResult;
    static void requestTask(void *arg);
    static void speechTask(void *arg);
    static void streamTestTask(void *arg);
    static void ttsTask(void *arg);
    static void httpEvent(lv_event_t *event);
    static void httpsEvent(lv_event_t *event);
    static void deepSeekEvent(lv_event_t *event);
    static void voiceEvent(lv_event_t *event);
    static void streamTestEvent(lv_event_t *event);
    static void speakEvent(lv_event_t *event);
    static void promptEvent(lv_event_t *event);
    static void keyboardEvent(lv_event_t *event);
    static void delayedDeepSeek(lv_timer_t *timer);
    static void delayedTts(lv_timer_t *timer);
    static void autoListenTimer(lv_timer_t *timer);
    static void listeningGlowTimer(lv_timer_t *timer);
    static void applyResultAsync(void *arg);
    void createKeyboard();
    void releaseKeyboard();
    void startRequest(RequestType type);
    void startSpeechRecognition();
    void startStreamTest();
    void finishListening();
    void startTts();
    void setListeningGlow(bool enabled);
    void scheduleAutoListen(uint32_t delay_ms);
    void setInteractionState(RobotInteractionState state);

    lv_obj_t *root_ = nullptr;
    lv_obj_t *status_label_ = nullptr;
    lv_obj_t *result_label_ = nullptr;
    lv_obj_t *http_button_ = nullptr;
    lv_obj_t *https_button_ = nullptr;
    lv_obj_t *deepseek_button_ = nullptr;
    lv_obj_t *voice_button_ = nullptr;
    lv_obj_t *speak_button_ = nullptr;
    lv_obj_t *prompt_textarea_ = nullptr;
    lv_obj_t *keyboard_ = nullptr;
    lv_obj_t *pinyin_ime_ = nullptr;
    lv_obj_t *glow_outer_ = nullptr;
    lv_obj_t *glow_middle_ = nullptr;
    lv_obj_t *glow_inner_ = nullptr;
    lv_obj_t *mic_button_ = nullptr;
    lv_timer_t *glow_timer_ = nullptr;
    lv_timer_t *auto_listen_timer_ = nullptr;
    uint16_t glow_phase_ = 0;
    TaskHandle_t request_task_ = nullptr;
    volatile bool listening_active_ = false;
    volatile bool finish_listening_requested_ = false;
    RequestType request_type_ = RequestType::Http;
    char prompt_[256] = "你好，请介绍一下你自己。";
    char last_answer_[512] = {};
    static constexpr size_t CONVERSATION_HISTORY_LIMIT = 3;
    char history_user_[CONVERSATION_HISTORY_LIMIT][256] = {};
    char history_assistant_[CONVERSATION_HISTORY_LIMIT][512] = {};
    size_t history_count_ = 0;
    volatile bool active_ = false;
    bool auto_listen_enabled_ = true;
    uint8_t consecutive_listen_failures_ = 0;
    RobotInteractionStateMachine interaction_;
};
