cmake_minimum_required(VERSION 3.22)

# Local, git-ignored overrides (Wi-Fi password) are layered on top of the defaults.
set(SDKCONFIG_DEFAULTS "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.secrets")
    list(APPEND SDKCONFIG_DEFAULTS "sdkconfig.secrets")
endif()

# Default chip for a fresh build dir; the ESP32-S3 builds with set-target (see README).
if(NOT DEFINED IDF_TARGET AND NOT DEFINED ENV{IDF_TARGET} AND NOT EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig")
    set(IDF_TARGET "esp32c3")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(fs-bms)
