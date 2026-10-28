# The authentication server: the device's key and TOTP secret, registration
# over HTTPS, the sign-in and the status report (server_logic.h is pure,
# host-tested).
idf_component_register(SRCS "server.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES crypto rng storefile ota nvs_flash mbedtls esp_http_client esp_app_format efuse esp_hw_support esp_timer heap freertos)
