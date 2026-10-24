# LVGL on the e-ink panel (portrait), the power-off wallpaper, the PIN pad, the
# screens with the status bar (ui_logic.h, wallpaper_logic.h, pin_pad_logic.h,
# nav_logic.h, list_logic.h and status_logic.h are pure) and the PSRAM allocator.
idf_component_register(SRCS "ui.cpp" "wallpaper.cpp" "pin_pad.cpp" "screens.cpp" "dialog.cpp" "lv_mem_psram.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl epd
                    PRIV_REQUIRES touch vault totp fido_ctap esp_timer heap freertos esp_app_format esp_hw_support vfs)

# lvgl calls lv_malloc_core & co. from lv_mem_psram.c: link it after lvgl too.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
