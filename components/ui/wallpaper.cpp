#include <dirent.h>
#include <sys/stat.h>

#include <cstdio>
#include <cstring>

#include "epd.h"
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

void PreviewTask(void *) {
    Lock();
    preview_err = ShowWallpaper(preview_grey);
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

}  // namespace

esp_err_t ShowWallpaper(bool grey) {
    if (!epd::Ready()) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    if (!Pick()) {
        return ESP_ERR_NOT_FOUND;
    }
    HoldPanel(true);  // LVGL's frames stay off it now
    size_t size = 0;
    uint8_t *png = ReadFile(&size);
    if (png == nullptr) {
        ESP_LOGE(TAG, "%s: cannot read (over %u KB?)", path,
                 static_cast<unsigned>(wallpaper::kMaxFileBytes / 1024));
        return ESP_FAIL;
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
        return ESP_FAIL;
    }
    uint8_t *rgba = decoded->data;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {  // grey in place
        const unsigned char *p = rgba + 4 * i;
        rgba[i] = wallpaper::Luma(p[0], p[1], p[2], p[3]);
    }
    // Two planes for grey (the level's low and high bit), one for B/W.
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
        dither.Run(rgba, w, h, grey ? epd::kGreyLevels : 2, screen[0], screen[1]);
        for (int i = 0; i < planes; ++i) {
            ToPanel(screen[i], frame[i]);
        }
        err = grey ? epd::RefreshGrey(frame[0], frame[1]) : epd::Refresh(frame[0], epd::Mode::kFull);
        ESP_LOGI(TAG, "%s (%ux%u, %s): %s", path, w, h, grey ? "4 levels" : "B/W",
                 esp_err_to_name(err));
    }
    lv_draw_buf_destroy(decoded);
    for (int i = 0; i < planes; ++i) {
        heap_caps_free(screen[i]);
        heap_caps_free(frame[i]);
    }
    return err;
}

esp_err_t PreviewWallpaper(bool grey, const char **shown) {
    preview_grey = grey;
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
