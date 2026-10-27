# The authentication server: the device's key and TOTP secret, registration
# over HTTPS (server_logic.h is pure, host-tested).
idf_component_register(SRCS "server.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES crypto rng mbedtls esp_http_client efuse esp_hw_support freertos)
