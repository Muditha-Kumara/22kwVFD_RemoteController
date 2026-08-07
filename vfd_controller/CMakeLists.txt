# vfd_controller — ESP32-S3 machine-side controller
cmake_minimum_required(VERSION 3.16)

# Shared components (vfd_protocol, sys_logger) live one level up.
set(EXTRA_COMPONENT_DIRS
    "${CMAKE_CURRENT_LIST_DIR}/../shared"
)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(vfd_controller)
