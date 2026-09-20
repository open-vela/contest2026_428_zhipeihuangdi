#pragma once

#include <stdbool.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

bool api_key_store_init(void);
bool api_key_store_get_speech(char *buf, size_t buf_len);
bool api_key_store_get_deepseek(char *buf, size_t buf_len);
bool api_key_store_set_speech(const char *key);
bool api_key_store_set_deepseek(const char *key);
bool api_key_store_has_keys(void);

#ifdef __cplusplus
}
#endif
