idf_component_register(SRCS "ble_fido.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES ble bt)

# NimBLE GATT tables set only the fields they use (zero-initialized rest).
target_compile_options(${COMPONENT_LIB} PRIVATE -Wno-missing-field-initializers)
