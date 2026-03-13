cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.1.0")

# Allow local overrides (WiFi creds, etc.) — not checked into git
if(EXISTS "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
    list(APPEND SDKCONFIG_DEFAULTS
         "${CMAKE_SOURCE_DIR}/sdkconfig.defaults"
         "${CMAKE_SOURCE_DIR}/sdkconfig.defaults.local")
endif()

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(arctic-sniffer)
