# rtc::Clock interface and its implementations; clock_math.h and
# bm8563_logic.h are pure (host-tested).
idf_component_register(SRCS "clock.cpp" "timer_clock.cpp" "bm8563_clock.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_hw_support i2c_bus
                    PRIV_REQUIRES esp_timer freertos)
