idf_component_register(SRCS "console.cpp" "files.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES console app_update esp_app_format hardware spiffs_fs
                             esp_driver_usb_serial_jtag)
