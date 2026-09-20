#include "StreamProtocol.hpp"

#include <cstdio>
#include <cstring>

#include "esp_crt_bundle.h"
#include "esp_log.h"

namespace vela::streaming {
namespace {
constexpr const char *TAG = "vela_stream_ws";
constexpr int FRAME_DURATION_MS = 60;
constexpr int SAMPLE_RATE = 16000;
}

StreamProtocol::~StreamProtocol() { stop(); }

esp_err_t StreamProtocol::start(const char *url, const char *authorization)
{
    if (!url || url[0] == '\0') return ESP_ERR_INVALID_ARG;
    if (client_) return ESP_ERR_INVALID_STATE;
    char headers[384] = {};
    if (authorization && authorization[0] != '\0') {
        snprintf(headers, sizeof(headers), "Authorization: %s\r\n", authorization);
    }
    esp_websocket_client_config_t config = {};
    config.uri = url;
    config.user_context = this;
    config.buffer_size = 4096;
    config.task_stack = 6144;
    config.network_timeout_ms = 15000;
    config.reconnect_timeout_ms = 3000;
    config.ping_interval_sec = 10;
    config.keep_alive_enable = true;
    config.crt_bundle_attach = esp_crt_bundle_attach;
    config.headers = headers[0] ? headers : nullptr;
    client_ = esp_websocket_client_init(&config);
    if (!client_) return ESP_ERR_NO_MEM;
    esp_err_t error = esp_websocket_register_events(
        client_, WEBSOCKET_EVENT_ANY, websocketEvent, this);
    if (error == ESP_OK) error = esp_websocket_client_start(client_);
    if (error != ESP_OK) stop();
    return error;
}

void StreamProtocol::stop()
{
    connected_ = false;
    if (!client_) return;
    esp_websocket_client_close(client_, pdMS_TO_TICKS(1000));
    esp_websocket_client_stop(client_);
    esp_websocket_client_destroy(client_);
    client_ = nullptr;
}

esp_err_t StreamProtocol::sendText(const char *text)
{
    if (!client_ || !connected_) return ESP_ERR_INVALID_STATE;
    const int length = static_cast<int>(strlen(text));
    return esp_websocket_client_send_text(client_, text, length,
                                           pdMS_TO_TICKS(1000)) == length
               ? ESP_OK : ESP_FAIL;
}

esp_err_t StreamProtocol::sendHello()
{
    char hello[256] = {};
    snprintf(hello, sizeof(hello),
             "{\"type\":\"hello\",\"version\":1,\"transport\":\"websocket\","
             "\"audio_params\":{\"format\":\"opus\",\"sample_rate\":%d,"
             "\"channels\":1,\"frame_duration\":%d}}",
             SAMPLE_RATE, FRAME_DURATION_MS);
    return sendText(hello);
}

esp_err_t StreamProtocol::startListening()
{
    return sendText("{\"type\":\"listen\",\"state\":\"start\",\"mode\":\"auto\"}");
}

esp_err_t StreamProtocol::stopListening()
{
    return sendText("{\"type\":\"listen\",\"state\":\"stop\"}");
}

esp_err_t StreamProtocol::sendOpus(const uint8_t *data, size_t length)
{
    if (!client_ || !connected_) return ESP_ERR_INVALID_STATE;
    if (!data || length == 0 || length > INT32_MAX) return ESP_ERR_INVALID_ARG;
    const int sent = esp_websocket_client_send_bin(
        client_, reinterpret_cast<const char *>(data), static_cast<int>(length),
        pdMS_TO_TICKS(1000));
    return sent == static_cast<int>(length) ? ESP_OK : ESP_FAIL;
}

void StreamProtocol::setEventCallback(EventCallback callback, void *context)
{
    event_callback_ = callback;
    event_context_ = context;
}

void StreamProtocol::setAudioCallback(AudioCallback callback, void *context)
{
    audio_callback_ = callback;
    audio_context_ = context;
}

void StreamProtocol::websocketEvent(void *handler_args, esp_event_base_t,
                                    int32_t event_id, void *event_data)
{
    auto *protocol = static_cast<StreamProtocol *>(handler_args);
    protocol->handleEvent(event_id,
                          static_cast<esp_websocket_event_data_t *>(event_data));
}

void StreamProtocol::handleEvent(int32_t event_id,
                                 esp_websocket_event_data_t *event)
{
    switch (event_id) {
    case WEBSOCKET_EVENT_CONNECTED:
        connected_ = true;
        ESP_LOGI(TAG, "WebSocket connected");
        sendHello();
        break;
    case WEBSOCKET_EVENT_DISCONNECTED:
    case WEBSOCKET_EVENT_CLOSED:
        connected_ = false;
        ESP_LOGW(TAG, "WebSocket disconnected");
        break;
    case WEBSOCKET_EVENT_DATA:
        if (!event || !event->data_ptr || event->data_len <= 0) break;
        if (event->op_code == 0x1 && event_callback_) {
            event_callback_(event->data_ptr, event->data_len, event_context_);
        } else if (event->op_code == 0x2 && audio_callback_) {
            audio_callback_(reinterpret_cast<const uint8_t *>(event->data_ptr),
                            event->data_len, audio_context_);
        }
        break;
    case WEBSOCKET_EVENT_ERROR:
        connected_ = false;
        ESP_LOGE(TAG, "WebSocket transport error");
        break;
    default:
        break;
    }
}

}  // namespace vela::streaming
