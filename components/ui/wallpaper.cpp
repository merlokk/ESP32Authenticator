#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "epd.h"
#include "esp_app_desc.h"
#include "esp_attr.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_random.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "lvgl.h"
// Its C API only: the C++ wrappers would land inside its extern "C".
#define LODEPNG_NO_COMPILE_CPP
#include "src/libs/lodepng/lodepng.h"
#include "ui.h"
#include "wallpaper_logic.h"

namespace ui {

namespace {

constexpr const char *TAG = "wallpaper";

EXT_RAM_BSS_ATTR wallpaper::Dither dither;  // 2.9 KB of error rows: static, not on a stack
EXT_RAM_BSS_ATTR char path[300];

// PreviewWallpaper: lodepng wants more stack than the console's 4 KB.
constexpr uint32_t kPreviewStackBytes = 6144;
SemaphoreHandle_t preview_done = nullptr;
esp_err_t preview_err = ESP_OK;
bool preview_grey = true;
bool preview_card = false;
esp_err_t ShowCard();

void PreviewTask(void *) {
    Lock();
    if (preview_card) {
        HoldPanel(true);
        preview_err = ShowCard();
    } else {
        preview_err = ShowWallpaper(preview_grey);
    }
    HoldPanel(false);
    Unlock();
    xSemaphoreGive(preview_done);
    vTaskDeleteWithCaps(nullptr);
}

// A random wallpaper's full path into `path`; false when there is none.
bool Pick() {
    DIR *dir = opendir(wallpaper::kDir);
    if (dir == nullptr) {
        return false;
    }
    uint32_t count = 0;
    for (dirent *e = readdir(dir); e != nullptr; e = readdir(dir)) {
        count += e->d_type != DT_DIR && wallpaper::IsImageName(e->d_name) ? 1 : 0;
    }
    bool found = false;
    if (count > 0) {
        uint32_t left = esp_random() % count;
        rewinddir(dir);
        for (dirent *e = readdir(dir); e != nullptr; e = readdir(dir)) {
            if (e->d_type == DT_DIR || !wallpaper::IsImageName(e->d_name)) {
                continue;
            }
            if (left-- == 0) {
                found = snprintf(path, sizeof path, "%s/%s", wallpaper::kDir, e->d_name) <
                        static_cast<int>(sizeof path);
                break;
            }
        }
    }
    closedir(dir);
    return found;
}

// The file whole, in PSRAM (heap_caps_free it); nullptr on any failure.
uint8_t *ReadFile(size_t *size) {
    struct stat st = {};
    if (stat(path, &st) != 0 || st.st_size <= 0 ||
        static_cast<size_t>(st.st_size) > wallpaper::kMaxFileBytes) {
        return nullptr;
    }
    auto *buf = static_cast<uint8_t *>(heap_caps_malloc(st.st_size, MALLOC_CAP_SPIRAM));
    FILE *f = buf != nullptr ? fopen(path, "rb") : nullptr;
    const bool ok = f != nullptr && fread(buf, 1, st.st_size, f) == static_cast<size_t>(st.st_size);
    if (f != nullptr) {
        fclose(f);
    }
    if (!ok) {
        heap_caps_free(buf);
        return nullptr;
    }
    *size = st.st_size;
    return buf;
}

// `grey_pixels` (w x h, a byte a pixel) dithered onto the glass, full
// refresh: 4 levels when `grey` and the panel has them, else B/W.
esp_err_t ToGlass(const uint8_t *grey_pixels, uint32_t w, uint32_t h, bool grey) {
    grey = grey && epd::HasGrey(epd::Detected());
    const int planes = grey ? 2 : 1;
    uint8_t *screen[2] = {};
    uint8_t *frame[2] = {};
    bool have = true;
    for (int i = 0; i < planes; ++i) {
        screen[i] = static_cast<uint8_t *>(heap_caps_malloc(kScreenBytes, MALLOC_CAP_SPIRAM));
        frame[i] = static_cast<uint8_t *>(heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
        have = have && screen[i] != nullptr && frame[i] != nullptr;
    }
    esp_err_t err = ESP_ERR_NO_MEM;
    if (have) {
        dither.Run(grey_pixels, w, h, grey ? epd::kGreyLevels : 2, screen[0], screen[1]);
        for (int i = 0; i < planes; ++i) {
            ToPanel(screen[i], frame[i]);
        }
        err = grey ? epd::RefreshGrey(frame[0], frame[1]) : epd::Refresh(frame[0], epd::Mode::kFull);
    }
    for (int i = 0; i < planes; ++i) {
        heap_caps_free(screen[i]);
        heap_caps_free(frame[i]);
    }
    return err;
}

// No wallpaper (none on the card, or it does not decode): the project's name
// and version on white, so an e-ink that is off does not look on. Drawn on
// an L8 canvas that is never shown; the caller holds Lock.
esp_err_t ShowCard() {
    snprintf(path, sizeof path, "(built in: the name and version)");
    lv_draw_buf_t *buf = lv_draw_buf_create(kWidth, kHeight, LV_COLOR_FORMAT_L8, LV_STRIDE_AUTO);
    if (buf == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    lv_obj_t *scratch = lv_obj_create(nullptr);  // a screen never loaded
    lv_obj_t *canvas = lv_canvas_create(scratch);
    lv_canvas_set_draw_buf(canvas, buf);
    lv_canvas_fill_bg(canvas, lv_color_white(), LV_OPA_COVER);
    lv_layer_t layer;
    lv_canvas_init_layer(canvas, &layer);
    const esp_app_desc_t *app = esp_app_get_description();
    struct Line {
        const lv_font_t *font;
        const char *text;
        int32_t y;
    };
    const Line lines[] = {
        {&lv_font_montserrat_32, "ESP32 Authenticator", 330},
        {&lv_font_montserrat_24, app->version, 386},
        {&lv_font_montserrat_20, "Off. Power turns it on.", 450},
    };
    for (const Line &line : lines) {
        lv_draw_label_dsc_t d;
        lv_draw_label_dsc_init(&d);
        d.font = line.font;
        d.color = lv_color_black();
        d.align = LV_TEXT_ALIGN_CENTER;
        d.text = line.text;
        const lv_area_t area = {0, line.y, kWidth - 1, line.y + 40};
        lv_draw_label(&layer, &d, &area);
    }
    lv_canvas_finish_layer(canvas, &layer);
    // Rows to a w x h array (the stride may pad them).
    const uint32_t stride = buf->header.stride;
    uint8_t *pixels = buf->data;
    for (int32_t y = 1; stride != static_cast<uint32_t>(kWidth) && y < kHeight; ++y) {
        memmove(pixels + y * kWidth, buf->data + y * stride, kWidth);
    }
    const esp_err_t err = ToGlass(pixels, kWidth, kHeight, false);
    lv_obj_delete(scratch);
    lv_draw_buf_destroy(buf);
    ESP_LOGI(TAG, "%s: %s", path, esp_err_to_name(err));
    return err;
}

}  // namespace

esp_err_t ShowWallpaper(bool grey) {
    if (!epd::Ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    HoldPanel(true);  // LVGL's frames stay off it now
    if (!Pick()) {
        return ShowCard();
    }
    size_t size = 0;
    uint8_t *png = ReadFile(&size);
    if (png == nullptr) {
        ESP_LOGE(TAG, "%s: cannot read (over %u KB?)", path,
                 static_cast<unsigned>(wallpaper::kMaxFileBytes / 1024));
        return ShowCard();
    }
    // LVGL's lodepng hands back an lv_draw_buf_t (PSRAM), not bare pixels:
    // RGBA in its data, 4 bytes a pixel, rows packed (1.5 MB for 480x800).
    unsigned char *out = nullptr;
    unsigned w = 0;
    unsigned h = 0;
    const unsigned lerr = lodepng_decode32(&out, &w, &h, png, size);
    heap_caps_free(png);
    auto *decoded = reinterpret_cast<lv_draw_buf_t *>(out);
    if (lerr != 0 || decoded == nullptr) {
        ESP_LOGE(TAG, "%s: %s", path, lodepng_error_text(lerr));
        return ShowCard();
    }
    uint8_t *rgba = decoded->data;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {  // grey in place
        const unsigned char *p = rgba + 4 * i;
        rgba[i] = wallpaper::Luma(p[0], p[1], p[2], p[3]);
    }
    const esp_err_t err = ToGlass(rgba, w, h, grey);
    ESP_LOGI(TAG, "%s (%ux%u, %s): %s", path, w, h, grey ? "grey" : "B/W", esp_err_to_name(err));
    lv_draw_buf_destroy(decoded);
    return err;
}

esp_err_t PreviewWallpaper(bool grey, const char **shown, bool card) {
    preview_grey = grey;
    preview_card = card;
    if (preview_done == nullptr) {
        preview_done = xSemaphoreCreateBinary();
    }
    // The stack in PSRAM: the task reads the SD card only, never the flash.
    if (xTaskCreateWithCaps(&PreviewTask, "wallpaper", kPreviewStackBytes, nullptr, 3, nullptr,
                            MALLOC_CAP_SPIRAM) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    xSemaphoreTake(preview_done, portMAX_DELAY);
    *shown = path;
    return preview_err;
}

}  // namespace ui
