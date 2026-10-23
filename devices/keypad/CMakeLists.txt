# The board's buttons in one polling task: events (key_events.h) and state.
idf_component_register(SRCS "keypad.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES boards buttons touch
                    PRIV_REQUIRES esp_timer freertos)
