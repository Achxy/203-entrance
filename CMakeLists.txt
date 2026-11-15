cmake_minimum_required(VERSION 3.22)
set(PROJECT_VER "0.1.0" CACHE STRING "Firmware semantic version")
set(ENTRANCE_TEST_FAULT "0" CACHE STRING "Qualification only: 0 none, 1 startup, 2 watchdog, 3 reset, 4 activation pause, 5 settings storage unavailable")
option(ENTRANCE_LIGHTS "Build the experimental single-board lighting gateway" OFF)
if(ENTRANCE_LIGHTS)
    list(APPEND EXTRA_COMPONENT_DIRS
        "${CMAKE_CURRENT_LIST_DIR}/lights/firmware/session"
        "${CMAKE_CURRENT_LIST_DIR}/lights/firmware/ble"
        "${CMAKE_CURRENT_LIST_DIR}/lights/firmware/wiz"
        "${CMAKE_CURRENT_LIST_DIR}/lights/firmware/gateway"
        "${CMAKE_CURRENT_LIST_DIR}/lights/firmware/runtime")
endif()
if(NOT ENTRANCE_TEST_FAULT MATCHES "^[0-5]$")
    message(FATAL_ERROR "Unknown qualification fault")
endif()
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
idf_build_set_property(ENTRANCE_LIGHTS ${ENTRANCE_LIGHTS})
project(203-entrance)
if(ENTRANCE_LIGHTS)
    foreach(name protocomm esp_security)
        idf_component_get_property(component_lib ${name} COMPONENT_LIB)
        target_compile_definitions(${component_lib} PRIVATE LOG_LOCAL_LEVEL=ESP_LOG_INFO)
    endforeach()
endif()
