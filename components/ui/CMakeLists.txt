# LVGL bring-up and its PSRAM allocator; the display and touch drivers attach later.
idf_component_register(SRCS "ui.cpp" "lv_mem_psram.c"
                    INCLUDE_DIRS "."
                    REQUIRES lvgl__lvgl
                    PRIV_REQUIRES esp_timer heap)

# lvgl calls lv_malloc_core & co. from lv_mem_psram.c: link it after lvgl too.
idf_component_get_property(lvgl_lib lvgl__lvgl COMPONENT_LIB)
target_link_libraries(${lvgl_lib} PUBLIC ${COMPONENT_LIB})
