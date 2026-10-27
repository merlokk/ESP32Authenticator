idf_component_register(SRCS "main.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES cli app_update battery sd_card keypad frontlight epd nvs_flash spiffs_fs ble ble_kb ble_fido config wifimgr web
                             fido_ctap ota boards timesync timezone ui vault crypto storefile power usb spi_flash esp_psram server)
