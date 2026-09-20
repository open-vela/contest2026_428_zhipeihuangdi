#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "esp_websocket_client.h"

namespace vela::streaming {

// Transport shape adapted from the open-source 78/xiaozhi-esp32 project.
// Board, display and codec ownership remain in the existing Vela application.
class StreamProtocol {
public:
    using EventCallback = void (*)(const char *json, size_t length, void *context);
    using AudioCallback = void (*)(const uint8_t *data, size_t length, void *context);

    StreamProtocol() = default;
    ~StreamProtocol();

    esp_err_t start(const char *url, const char *authorization = nullptr);
    void stop();
    bool connected() const { return connected_; }
    esp_err_t sendHello();
    esp_err_t startListening();
    esp_err_t stopListening();
    esp_err_t sendOpus(const uint8_t *data, size_t length);
    void setEventCallback(EventCallback callback, void *context);
    void setAudioCallback(AudioCallback callback, void *context);

private:
    static void websocketEvent(void *handler_args, esp_event_base_t base,
                               int32_t event_id, void *event_data);
    void handleEvent(int32_t event_id, esp_websocket_event_data_t *event);
    esp_err_t sendText(const char *text);

    esp_websocket_client_handle_t client_ = nullptr;
    bool connected_ = false;
    EventCallback event_callback_ = nullptr;
    AudioCallback audio_callback_ = nullptr;
    void *event_context_ = nullptr;
    void *audio_context_ = nullptr;
};

}  // namespace vela::streaming
