# rtc::Clock interface and its implementations; clock_math.h is pure (host-tested).
idf_component_register(SRCS "clock.cpp" "timer_clock.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES esp_hw_support)
