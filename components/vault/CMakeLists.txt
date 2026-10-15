# Password store: records.cpp and pin_guard.cpp are pure (host-tested); vault.cpp loads, encrypts
# and saves them.
idf_component_register(SRCS "records.cpp" "pin_guard.cpp" "vault.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_common
                    PRIV_REQUIRES totp storefile crypto clock nvs_flash heap freertos esp_timer pin_policy)
