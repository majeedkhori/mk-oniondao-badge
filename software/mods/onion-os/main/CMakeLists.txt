idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espressif__arduino-esp32 GxEPD2 mqtt json esp_http_client esp-tls lua espressif__libsodium espressif__esp-cryptoauthlib mbedtls
)

target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_HAS_PSRAM LUA_32BITS)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
