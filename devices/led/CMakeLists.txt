# WS2812 on a UART; led_frames.cpp is pure (host-tested).
idf_component_register(SRCS "led.cpp" "led_frames.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_uart esp_driver_gpio
                    PRIV_REQUIRES esp_timer)
