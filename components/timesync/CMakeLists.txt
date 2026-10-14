# sync_policy.cpp is pure (host-tested); timesync.cpp runs the SNTP exchange.
idf_component_register(SRCS "sync_policy.cpp" "timesync.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES clock
                    PRIV_REQUIRES config wifimgr wifi esp_netif esp_timer)
