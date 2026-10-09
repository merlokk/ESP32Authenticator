idf_component_register(SRCS "console.cpp" "files.cpp" "ble_cmd.cpp" "wifi_cmd.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console app_update esp_app_format hardware spiffs_fs
                             esp_driver_usb_serial_jtag esp_partition ble ble_kb ble_fido wifi)
