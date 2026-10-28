# Firmware version: <tag>-<commits since it>-g<hash>[-dirty] from the nearest
# x.y.z git tag, e.g. 0.2.0-3-gabc1234; with no tag <version.txt>-<commits>-g<hash>.
# Sets PROJECT_VER, which ESP-IDF stores in esp_app_desc_t.version (max 31 chars).
# Evaluated at CMake configure time: run `idf.py reconfigure` to refresh it.

file(STRINGS "${CMAKE_CURRENT_LIST_DIR}/../version.txt" FW_VERSION_BASE LIMIT_COUNT 1)

find_package(Git QUIET)
if(GIT_FOUND)
    execute_process(COMMAND ${GIT_EXECUTABLE} describe --tags --long --abbrev=7
                            --match "[0-9]*.[0-9]*.[0-9]*"
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                    OUTPUT_VARIABLE FW_GIT_DESCRIBE OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-list --count HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                    OUTPUT_VARIABLE FW_GIT_COUNT OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} rev-parse --short=7 HEAD
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                    OUTPUT_VARIABLE FW_GIT_HASH OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
    execute_process(COMMAND ${GIT_EXECUTABLE} status --porcelain --untracked-files=no
                    WORKING_DIRECTORY ${CMAKE_CURRENT_LIST_DIR}/..
                    OUTPUT_VARIABLE FW_GIT_STATUS OUTPUT_STRIP_TRAILING_WHITESPACE ERROR_QUIET)
endif()

if(FW_GIT_DESCRIBE MATCHES "^[0-9]+\\.[0-9]+\\.[0-9]+-[0-9]+-g[0-9a-f]+$")
    set(PROJECT_VER "${FW_GIT_DESCRIBE}")
elseif(FW_GIT_HASH)
    set(PROJECT_VER "${FW_VERSION_BASE}-${FW_GIT_COUNT}-g${FW_GIT_HASH}")
else()
    set(PROJECT_VER "${FW_VERSION_BASE}-nogit")
endif()
if(FW_GIT_HASH AND FW_GIT_STATUS)
    string(APPEND PROJECT_VER "-dirty")
endif()

message(STATUS "Firmware version: ${PROJECT_VER}")
