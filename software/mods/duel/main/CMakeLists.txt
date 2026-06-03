idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espressif__arduino-esp32 GxEPD2 Adafruit_GFX Adafruit_BusIO SparkFun_ATECCX08a
)
target_compile_definitions(${COMPONENT_LIB} PRIVATE BOARD_HAS_PSRAM)
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation)
