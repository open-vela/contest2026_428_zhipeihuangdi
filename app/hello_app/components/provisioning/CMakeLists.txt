idf_component_register(
    SRCS "provisioning.c"
    INCLUDE_DIRS "."
    REQUIRES api_key_store esp_wifi esp_http_server nvs_flash
)
