# image_check is pure (host-tested); ota_update is the esp_ota side.
idf_component_register(SRCS "image_check.cpp" "ota_update.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES app_update esp_app_format esp_timer hal sd_card)
