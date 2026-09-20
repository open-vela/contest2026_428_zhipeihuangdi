#include "StreamOpusEncoder.hpp"

#include "encoder/esp_audio_enc.h"
#include "encoder/impl/esp_opus_enc.h"
#include "esp_log.h"

namespace vela::streaming {

StreamOpusEncoder::~StreamOpusEncoder() { deinit(); }

esp_err_t StreamOpusEncoder::init()
{
    if (handle_) return ESP_OK;
    esp_opus_enc_config_t config = ESP_OPUS_ENC_CONFIG_DEFAULT();
    config.sample_rate = 16000;
    config.channel = 1;
    config.bits_per_sample = 16;
    config.bitrate = 24000;
    config.frame_duration = ESP_OPUS_ENC_FRAME_DURATION_60_MS;
    config.application_mode = ESP_OPUS_ENC_APPLICATION_VOIP;
    config.complexity = 0;
    config.enable_dtx = true;
    config.enable_vbr = true;
    const esp_audio_err_t open_result =
        esp_opus_enc_open(&config, sizeof(config), &handle_);
    if (open_result != ESP_AUDIO_ERR_OK || !handle_) {
        ESP_LOGE("vela_opus", "Encoder open failed: %d", open_result);
        handle_ = nullptr;
        return ESP_FAIL;
    }
    int input_bytes = 0;
    int output_bytes = 0;
    if (esp_opus_enc_get_frame_size(handle_, &input_bytes, &output_bytes) !=
        ESP_AUDIO_ERR_OK) {
        deinit();
        return ESP_FAIL;
    }
    pcm_bytes_per_frame_ = static_cast<size_t>(input_bytes);
    max_opus_bytes_ = static_cast<size_t>(output_bytes);
    return ESP_OK;
}

void StreamOpusEncoder::deinit()
{
    if (handle_) esp_opus_enc_close(handle_);
    handle_ = nullptr;
    pcm_bytes_per_frame_ = 0;
    max_opus_bytes_ = 0;
}

esp_err_t StreamOpusEncoder::encode(const int16_t *pcm, size_t samples,
                                    uint8_t *opus, size_t opus_capacity,
                                    size_t *opus_length)
{
    if (!handle_ || !pcm || !opus || !opus_length) return ESP_ERR_INVALID_ARG;
    const size_t pcm_bytes = samples * sizeof(int16_t);
    if (pcm_bytes != pcm_bytes_per_frame_ || opus_capacity < max_opus_bytes_) {
        return ESP_ERR_INVALID_SIZE;
    }
    esp_audio_enc_in_frame_t input = {};
    input.buffer = reinterpret_cast<uint8_t *>(const_cast<int16_t *>(pcm));
    input.len = static_cast<uint32_t>(pcm_bytes);
    esp_audio_enc_out_frame_t output = {};
    output.buffer = opus;
    output.len = static_cast<uint32_t>(opus_capacity);
    if (esp_opus_enc_process(handle_, &input, &output) != ESP_AUDIO_ERR_OK) {
        return ESP_FAIL;
    }
    *opus_length = output.encoded_bytes;
    return ESP_OK;
}

}  // namespace vela::streaming
