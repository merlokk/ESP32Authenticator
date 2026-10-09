idf_component_register(SRCS "wifi.cpp"
                    INCLUDE_DIRS "."
                    PRIV_REQUIRES esp_wifi esp_netif esp_event esp_timer)
