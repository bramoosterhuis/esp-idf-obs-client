idf_component_register(
    SRCS "src/obs-client.c"
    INCLUDE_DIRS "include"
    REQUIRES json log nvs_flash
    PRIV_REQUIRES esp_timer esp_websocket_client log mbedtls)