#pragma once

#include <cstddef>
#include <cstdint>

#include "esp_err.h"

namespace vela::streaming {

class StreamOpusEncoder {
public:
    StreamOpusEncoder() = default;
    ~StreamOpusEncoder();
    esp_err_t init();
    void deinit();
    size_t pcmBytesPerFrame() const { return pcm_bytes_per_frame_; }
    size_t maxOpusBytes() const { return max_opus_bytes_; }
    esp_err_t encode(const int16_t *pcm, size_t samples, uint8_t *opus,
                     size_t opus_capacity, size_t *opus_length);

private:
    void *handle_ = nullptr;
    size_t pcm_bytes_per_frame_ = 0;
    size_t max_opus_bytes_ = 0;
};

}  // namespace vela::streaming
