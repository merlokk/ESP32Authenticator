idf_component_register(SRCS "ble.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES bt
                    PRIV_REQUIRES esp_app_format esp_timer)

# NimBLE GATT tables set only the fields they use (zero-initialized rest).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
