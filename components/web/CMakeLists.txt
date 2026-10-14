# web_auth, web_paths, web_policy and web_settings are pure (host-tested);
# web_server is the esp_http_server side. Pages are embedded (www/).
idf_component_register(SRCS "web_auth.cpp" "web_paths.cpp" "web_policy.cpp" "web_settings.cpp"
                            "web_server.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES config
                    PRIV_REQUIRES esp_http_server esp_app_format esp_timer wifimgr wifi spiffs_fs
                                  ble ble_kb ble_fido fido_ctap bt espressif__cjson timesync timezone
                    EMBED_FILES "www/index.html" "www/wifi.html" "www/devstatus.html"
                                "www/reboot.html" "www/404.html" "www/401.html" "www/app.js")
