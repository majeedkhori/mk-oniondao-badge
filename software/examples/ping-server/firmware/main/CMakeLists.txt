idf_component_register(
    SRCS "main.cpp"
    INCLUDE_DIRS "."
    REQUIRES espressif__arduino-esp32 GxEPD2
)

target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-error=format-truncation -Wno-error=format)
