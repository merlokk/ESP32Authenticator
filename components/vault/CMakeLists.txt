# Password store: records.cpp, pin_guard.cpp and seed_phrase.cpp are pure (host-tested);
# vault.cpp loads, encrypts and saves them.
idf_component_register(SRCS "records.cpp" "pin_guard.cpp" "seed_phrase.cpp" "seed_words.cpp" "vault.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_common storefile
                    PRIV_REQUIRES totp crypto clock nvs_flash heap freertos esp_timer pin_policy)
