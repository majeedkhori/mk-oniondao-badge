# OnionDAO Badge — Duel mod (ESP-IDF + arduino-esp32)
# ESP32-S3-WROOM-1-N8R8 (8 MB flash, 8 MB OPI PSRAM)
cmake_minimum_required(VERSION 3.16)

# Shared Arduino-library components (GxEPD2, Adafruit_GFX, Adafruit_BusIO)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../../components")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(duel)
