# remote_control — handheld ESP-NOW remote for the 22kW cutter (ESP32)
cmake_minimum_required(VERSION 3.16)

# Shared components live one level up.
set(EXTRA_COMPONENT_DIRS "${CMAKE_CURRENT_LIST_DIR}/../shared")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(remote_control)
