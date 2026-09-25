cmake_minimum_required(VERSION 3.20)
set(PROJECT_VER "0.4.0")
# The compatibility family preserves the deployed signed-OTA application gate.
# It does not change the converter's user-facing application identity.
set(GW_PROJECT_FAMILY "ats_modbus_bacnet" CACHE STRING "Signed OTA application family")
if(NOT GW_PROJECT_FAMILY STREQUAL "ats_modbus_bacnet" AND
   NOT GW_PROJECT_FAMILY STREQUAL "esp32_p4_bacnet_switches")
    message(FATAL_ERROR "Unsupported signed OTA application family")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(${GW_PROJECT_FAMILY})
