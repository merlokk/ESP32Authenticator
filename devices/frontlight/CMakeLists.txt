# The frontlight: two PWM strings (cool, warm); frontlight_logic.h is pure (host-tested).
idf_component_register(SRCS "frontlight.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES boards esp_driver_ledc freertos)
