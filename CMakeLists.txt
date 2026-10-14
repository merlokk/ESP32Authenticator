cmake_minimum_required(VERSION 3.22)

include(${CMAKE_CURRENT_LIST_DIR}/cmake/version.cmake)

# Hardware drivers live in devices/, one component per device.
set(EXTRA_COMPONENT_DIRS ${CMAKE_CURRENT_LIST_DIR}/devices)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(esp32-authenticator)
