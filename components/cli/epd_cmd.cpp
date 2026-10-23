#include "epd_cmd.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "epd.h"
#include "esp_heap_caps.h"

namespace console {

namespace {

// The test frame, in PSRAM: 48 KB is no stack's business.
uint8_t *frame = nullptr;

void Black(int x, int y) {
    frame[y * epd::kRowBytes + x / 8] &= static_cast<uint8_t>(~(0x80 >> (x % 8)));
}

void Rect(int x0, int y0, int w, int h) {
    for (int y = y0; y < y0 + h; ++y) {
        for (int x = x0; x < x0 + w; ++x) {
            Black(x, y);
        }
    }
}

bool Draw(const char *pattern) {
    if (strcmp(pattern, "white") == 0) {
        memset(frame, 0xFF, epd::kFrameBytes);
    } else if (strcmp(pattern, "black") == 0) {
        memset(frame, 0x00, epd::kFrameBytes);
    } else if (strcmp(pattern, "checker") == 0) {
        memset(frame, 0xFF, epd::kFrameBytes);
        for (int y = 0; y < epd::kHeight; y += 40) {
            for (int x = 0; x < epd::kWidth; x += 40) {
                if (((x / 40) + (y / 40)) % 2 == 0) {
                    Rect(x, y, 40, 40);
                }
            }
        }
    } else if (strcmp(pattern, "corners") == 0) {
        // A 60 px square in each corner, flush with the frame's edges, a
        // 20 px white notch at its inner corner: what shows of each says how
        // much of the frame is cut off there.
        memset(frame, 0xFF, epd::kFrameBytes);
        const int s = 60;
        Rect(0, 0, s, s);
        Rect(epd::kWidth - s, 0, s, s);
        Rect(0, epd::kHeight - s, s, s);
        Rect(epd::kWidth - s, epd::kHeight - s, s, s);
        Rect(epd::kWidth / 2 - 100, epd::kHeight / 2 - 20, 200, 40);  // the middle: it refreshed
        for (int y = 0; y < 20; ++y) {  // the notches, back to white
            for (int x = 0; x < 20; ++x) {
                const int xs[] = {s - 20 + x, epd::kWidth - s + x};
                const int ys[] = {s - 20 + y, epd::kHeight - s + y};
                for (const int px : xs) {
                    for (const int py : ys) {
                        frame[py * epd::kRowBytes + px / 8] |= static_cast<uint8_t>(0x80 >> (px % 8));
                    }
                }
            }
        }
    } else if (strcmp(pattern, "edges") == 0) {
        // How much the bezel hides: 2 px lines every 4 px from each edge
        // (offsets 0, 4, .. 24): the lines not seen times 4 is the inset.
        memset(frame, 0xFF, epd::kFrameBytes);
        for (int d = 0; d <= 24; d += 4) {
            Rect(d, d, epd::kWidth - 2 * d, 2);                    // top
            Rect(d, epd::kHeight - d - 2, epd::kWidth - 2 * d, 2);  // bottom
            Rect(d, d, 2, epd::kHeight - 2 * d);                   // left
            Rect(epd::kWidth - d - 2, d, 2, epd::kHeight - 2 * d);  // right
        }
        Rect(epd::kWidth / 2 - 100, epd::kHeight / 2 - 20, 200, 40);  // the middle: it refreshed
    } else if (strcmp(pattern, "orient") == 0) {
        // Which way is up: a big square top left, a small one top right, a
        // bar along the bottom, a 12 px border (the bezel hides a few px).
        memset(frame, 0xFF, epd::kFrameBytes);
        Rect(0, 0, epd::kWidth, 12);
        Rect(0, epd::kHeight - 12, epd::kWidth, 12);
        Rect(0, 0, 12, epd::kHeight);
        Rect(epd::kWidth - 12, 0, 12, epd::kHeight);
        Rect(40, 40, 160, 160);
        Rect(epd::kWidth - 120, 40, 80, 80);
        Rect(200, epd::kHeight - 80, epd::kWidth - 400, 30);
    } else {
        return false;
    }
    return true;
}

}  // namespace

int CmdEpd(int argc, char **argv) {
    if (argc == 1 || (argc == 2 && strcmp(argv[1], "info") == 0)) {
        epd::PrintInfo();
        return 0;
    }
    if (!epd::Ready()) {
        epd::PrintInfo();
        return 1;
    }
    if (argc == 2 && strcmp(argv[1], "sleep") == 0) {
        const esp_err_t err = epd::Sleep();
        printf("%s\n", err == ESP_OK ? "asleep" : esp_err_to_name(err));
        return err == ESP_OK ? 0 : 1;
    }
    if (strcmp(argv[1], "test") != 0 || argc > 4) {
        printf("usage: epd [info] | epd test [orient|corners|edges|checker|black|white] [full|fast] | epd sleep\n");
        return 1;
    }
    const char *pattern = argc >= 3 ? argv[2] : "orient";
    epd::Mode mode = epd::Mode::kFull;
    if (argc == 4) {
        if (strcmp(argv[3], "fast") == 0) {
            mode = epd::Mode::kFast;
        } else if (strcmp(argv[3], "full") != 0) {
            printf("mode: full or fast\n");
            return 1;
        }
    }
    if (frame == nullptr) {
        frame = static_cast<uint8_t *>(heap_caps_malloc(epd::kFrameBytes, MALLOC_CAP_SPIRAM));
        if (frame == nullptr) {
            printf("no memory for a frame\n");
            return 1;
        }
    }
    if (!Draw(pattern)) {
        printf("pattern: orient, corners, edges, checker, black or white\n");
        return 1;
    }
    const esp_err_t err = epd::Refresh(frame, mode);
    if (err != ESP_OK) {
        printf("refresh: %s\n", esp_err_to_name(err));
        return 1;
    }
    epd::PrintInfo();
    return 0;
}

}  // namespace console
