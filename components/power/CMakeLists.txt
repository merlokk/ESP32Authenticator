# Power-off: wallpaper, deep sleep, wake on Power; the idle watch; a secret
# kept through a sleep (power_logic.h is pure).
idf_component_register(SRCS "power.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES boards
                    PRIV_REQUIRES ui config epd frontlight keypad ble battery clock esp_driver_gpio
                                  usb esp_hw_support esp_system esp_timer freertos)
