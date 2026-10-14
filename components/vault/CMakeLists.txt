# Password store: records.cpp is pure (host-tested); vault.cpp loads, encrypts
# and saves them.
idf_component_register(SRCS "records.cpp" "vault.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_common
                    PRIV_REQUIRES totp storefile crypto clock nvs_flash heap freertos)
