# The board's I2C bus, leased (RTC, touch, fuel gauge share it). Ported from
# approver-esp32 (components/i2cbus).
idf_component_register(SRCS "i2c_bus.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_driver_i2c esp_driver_gpio)
