# The touch controller (GT911); gt911_logic.h is pure (host-tested).
idf_component_register(SRCS "touch.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_gpio
                    PRIV_REQUIRES boards i2c_bus freertos)
