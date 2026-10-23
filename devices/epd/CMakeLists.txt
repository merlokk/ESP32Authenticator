# The e-ink panel (SSD1677 / UC8179 / UC8279); epd_logic.h is pure (host-tested).
idf_component_register(SRCS "epd.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES boards esp_driver_spi esp_driver_gpio esp_timer freertos nvs_flash)
