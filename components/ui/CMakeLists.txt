# LVGL on the e-ink panel (portrait, ui_logic.h is pure) and its PSRAM allocator.
idf_component_register(SRCS "ui.cpp" "lv_mem_psram.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl epd
                    PRIV_REQUIRES esp_timer heap freertos esp_app_format)

# lvgl calls lv_malloc_core & co. from lv_mem_psram.c: link it after lvgl too.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
