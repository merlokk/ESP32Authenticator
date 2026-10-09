idf_component_register(SRCS "hardware.cpp" "efuse_dump.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_tsens app_update efuse spi_flash esp_psram esp_partition nvs_flash
                                  bootloader_support esp_timer)

# Full eFuse field list, generated from the IDF table for the current target.
idf_build_get_property(python PYTHON)
idf_build_get_property(idf_path IDF_PATH)
set(efuse_csv ${idf_path}/components/efuse/${IDF_TARGET}/esp_efuse_table.csv)
set(efuse_inc ${CMAKE_CURRENT_BINARY_DIR}/efuse_fields.inc)
add_custom_command(OUTPUT ${efuse_inc}
                   COMMAND ${python} ${CMAKE_CURRENT_LIST_DIR}/gen_efuse_fields.py ${efuse_csv} ${efuse_inc}
                   DEPENDS ${CMAKE_CURRENT_LIST_DIR}/gen_efuse_fields.py ${efuse_csv}
                   VERBATIM)
add_custom_target(efuse_fields DEPENDS ${efuse_inc})
add_dependencies(${COMPONENT_LIB} efuse_fields)
target_include_directories(${COMPONENT_LIB} PRIVATE ${CMAKE_CURRENT_BINARY_DIR})
