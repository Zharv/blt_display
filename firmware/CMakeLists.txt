cmake_minimum_required(VERSION 3.16)

# Wi-Fi/printer credentials live in a local, gitignored sdkconfig.credentials
# (copy sdkconfig.credentials.example). Loaded after sdkconfig.defaults when present.
set(sdkdefaults "sdkconfig.defaults")
if(EXISTS "${CMAKE_CURRENT_LIST_DIR}/sdkconfig.credentials")
    list(APPEND sdkdefaults "sdkconfig.credentials")
endif()
set(SDKCONFIG_DEFAULTS "${sdkdefaults}")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(blt_esp_display)
