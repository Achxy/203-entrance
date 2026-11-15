cmake_minimum_required(VERSION 3.20)
get_filename_component(REPO "${CMAKE_CURRENT_LIST_DIR}/../../../../.." ABSOLUTE)
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../.."
    "${REPO}/lights/firmware/wiz" "${REPO}/managed_components/espressif__cjson")
set(COMPONENTS main gateway)
include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(lights-gateway-compile-check)
