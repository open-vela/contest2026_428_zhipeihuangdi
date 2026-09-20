#include "api_key_store.h"
#include "nvs.h"
#include "esp_log.h"
#include <string.h>

static const char *TAG = "api_key_store";
static const char *NVS_NAMESPACE = "api_keys";
static const char *NVS_KEY_SPEECH = "speech_key";
static const char *NVS_KEY_DEEPSEEK = "ds_key";

bool api_key_store_init(void)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err == ESP_OK) {
        nvs_close(handle);
        return true;
    }
    return false;
}

static bool store_get_str(const char *key, char *buf, size_t buf_len)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READONLY, &handle);
    if (err != ESP_OK) return false;
    size_t len = buf_len;
    err = nvs_get_str(handle, key, buf, &len);
    nvs_close(handle);
    return (err == ESP_OK && len > 1);
}

static bool store_set_str(const char *key, const char *value)
{
    nvs_handle_t handle;
    esp_err_t err = nvs_open(NVS_NAMESPACE, NVS_READWRITE, &handle);
    if (err != ESP_OK) return false;
    err = nvs_set_str(handle, key, value);
    if (err == ESP_OK) err = nvs_commit(handle);
    nvs_close(handle);
    return (err == ESP_OK);
}

bool api_key_store_get_speech(char *buf, size_t buf_len)
{
    return store_get_str(NVS_KEY_SPEECH, buf, buf_len);
}

bool api_key_store_get_deepseek(char *buf, size_t buf_len)
{
    return store_get_str(NVS_KEY_DEEPSEEK, buf, buf_len);
}

bool api_key_store_set_speech(const char *key)
{
    ESP_LOGI(TAG, "Saving speech API key");
    return store_set_str(NVS_KEY_SPEECH, key);
}

bool api_key_store_set_deepseek(const char *key)
{
    ESP_LOGI(TAG, "Saving deepseek API key");
    return store_set_str(NVS_KEY_DEEPSEEK, key);
}

bool api_key_store_has_keys(void)
{
    char buf[128];
    return store_get_str(NVS_KEY_SPEECH, buf, sizeof(buf)) &&
           store_get_str(NVS_KEY_DEEPSEEK, buf, sizeof(buf));
}
