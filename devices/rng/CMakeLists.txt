# The hardware random number generator; rng_logic.h is pure (host-tested).
idf_component_register(SRCS "rng.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_hw_support)
