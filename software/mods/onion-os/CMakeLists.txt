# OnionDAO Badge — Onion OS (ESP-IDF + arduino-esp32)
# ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB OPI PSRAM)
cmake_minimum_required(VERSION 3.16)

set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(onion-os)

# Real-time Lua (e.g. a 16 kHz mic DSP/codec loop) is interpreter-bound: at the
# project default -Og the VM sustained only ~9k samples/s and the PDM ring
# overflowed. -O2 on the interpreter alone buys ~1.5-2x and leaves the rest of
# the firmware at -Og for debuggability.
idf_component_get_property(lua_lib espressif__lua COMPONENT_LIB)
if(lua_lib)
    target_compile_options(${lua_lib} PRIVATE -O2)
endif()
