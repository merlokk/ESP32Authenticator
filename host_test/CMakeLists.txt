# Host tests: code that needs no board, built by the host compiler (MSVC via
# run.cmd). Not ESP-IDF's `linux` target: on a Windows host it cannot link
# (approver-esp32 measured this).
#
# Unity comes from the ESP-IDF checkout, cJSON from managed_components/
# (run `idf.py build` once to fetch it). `fakes/` shadows the few ESP-IDF
# headers the code under test includes.

cmake_minimum_required(VERSION 3.22)
project(esp32_authenticator_host_test C CXX)

set(CMAKE_C_STANDARD 11)
# C++20: designated initializers, which the firmware uses (gnu++26 there).
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT DEFINED IDF_PATH)
    set(IDF_PATH "$ENV{IDF_PATH}")
endif()
set(UNITY_DIR "${IDF_PATH}/components/unity/unity/src")

if(NOT DEFINED CJSON_DIR)
    set(CJSON_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../managed_components/espressif__cjson/cJSON")
endif()

if(NOT EXISTS "${CJSON_DIR}/cJSON.c")
    message(FATAL_ERROR "cJSON not found at ${CJSON_DIR}: run `idf.py build` once.")
endif()
if(NOT EXISTS "${UNITY_DIR}/unity.c")
    message(FATAL_ERROR "Unity not found at ${UNITY_DIR}: pass -DIDF_PATH=<esp-idf>.")
endif()

add_executable(host_test
    test_main.cpp
    test_config.cpp
    fakes/fake_platform.cpp

    # Under test.
    ../components/config/config.cpp

    "${CJSON_DIR}/cJSON.c"
    "${UNITY_DIR}/unity.c"
)

target_include_directories(host_test PRIVATE
    .
    fakes  # before anything that could be real ESP-IDF
    ../components/config
    "${CJSON_DIR}"
    "${UNITY_DIR}"
)

target_compile_definitions(host_test PRIVATE UNITY_INCLUDE_DOUBLE)

if(MSVC)
    # fopen & co. are "deprecated" to MSVC; the firmware builds the same lines
    # with GCC -Wall -Wextra -Werror.
    target_compile_definitions(host_test PRIVATE _CRT_SECURE_NO_WARNINGS _CRT_NONSTDC_NO_WARNINGS)
    target_compile_options(host_test PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:/W4;/WX>")
else()
    target_compile_options(host_test PRIVATE "$<$<COMPILE_LANGUAGE:CXX>:-Wall;-Wextra;-Werror>")
endif()
