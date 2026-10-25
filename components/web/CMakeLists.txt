# web_auth, web_paths, web_policy, web_settings and web_admin are pure (host-tested);
# web_server, web_admin_routes and web_firmware are the esp_http_server side. Pages are embedded (www/).
idf_component_register(SRCS "web_auth.cpp" "web_paths.cpp" "web_policy.cpp" "web_settings.cpp"
                            "web_admin.cpp" "web_server.cpp" "web_admin_routes.cpp" "web_firmware.cpp"
                    INCLUDE_DIRS "."
                    REQUIRES config
                    PRIV_REQUIRES esp_http_server esp_app_format esp_timer wifimgr wifi spiffs_fs
                                  ble ble_kb ble_fido fido_ctap bt espressif__cjson timesync timezone boards
                                  vault totp clock frontlight sd_card ota
                    EMBED_FILES "www/index.html" "www/wifi.html" "www/devstatus.html" "www/hwinfo.html"
                                "www/reboot.html" "www/404.html" "www/401.html" "www/app.js"
                                "www/passwords.html" "www/fido.html" "www/settings.html"
                                "www/firmware.html")
