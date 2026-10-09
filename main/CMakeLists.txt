idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES cli app_update nvs_flash spiffs_fs ble ble_kb ble_fido config wifimgr)
