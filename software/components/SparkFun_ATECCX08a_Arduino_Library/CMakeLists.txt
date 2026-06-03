# SparkFun ATECCX08a Arduino Library
# https://github.com/sparkfun/SparkFun_ATECCX08a_Arduino_Library
# MIT License

cmake_minimum_required(VERSION 3.5)

# REQUIRES the arduino-esp32 managed component (espressif__arduino-esp32).
idf_component_register(SRCS "SparkFun_ATECCX08a_Arduino_Library.cpp"
                       INCLUDE_DIRS "."
                       REQUIRES espressif__arduino-esp32)
