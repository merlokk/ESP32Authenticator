idf_component_register(SRCS "console.cpp" "files.cpp" "ble_cmd.cpp" "wifi_cmd.cpp" "fido_cmd.cpp" "date_cmd.cpp" "totp_cmd.cpp" "pass_cmd.cpp" "nvs_cmd.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console app_update battery esp_app_format hardware spiffs_fs
                             esp_driver_usb_serial_jtag esp_partition esp_netif ble ble_kb ble_fido
                             wifi config wifimgr web fido_ctap boards clock led timesync timezone totp crypto ui vault nvs_flash)
