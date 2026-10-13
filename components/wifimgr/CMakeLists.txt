# wifi_policy.cpp and reachability.cpp include only <cstdint>: the host tests compile it as is.
idf_component_register(SRCS "wifi_policy.cpp" "reachability.cpp" "wifi_manager.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES wifi
                    PRIV_REQUIRES config esp_timer lwip)
