# Power-off: wallpaper, deep sleep, wake on Power (power_logic.h is pure).
idf_component_register(SRCS "power.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES boards
                    PRIV_REQUIRES ui epd frontlight keypad esp_driver_gpio esp_hw_support freertos)
