# Battery: the CW2017 fuel gauge (voltage, level) and the charger STAT line;
# cw2017_logic.h is pure (host-tested).
idf_component_register(SRCS "battery.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio
                    PRIV_REQUIRES boards i2c_bus freertos)
