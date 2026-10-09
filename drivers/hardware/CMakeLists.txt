idf_component_register(SRCS "hardware.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_driver_tsens app_update efuse spi_flash esp_psram esp_partition nvs_flash
                                  bootloader_support esp_timer)
