#include "epd.h"

#include <cinttypes>
#include <cstdio>
#include <cstring>

#include "board.h"
#include "driver/gpio.h"
#include "driver/spi_master.h"
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_rom_sys.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "nvs.h"

namespace epd {

namespace {

constexpr const char *TAG = "epd";
constexpr spi_host_device_t kHost = SPI2_HOST;
// Rows per SPI transfer through the DMA bounce buffer (4000 bytes).
constexpr size_t kChunkRows = 40;

struct Pins {
    gpio_num_t mosi, sclk, cs, dc, rst, busy;
};

Pins pins;
Controller controller = Controller::kUnknown;
bool ready = false;
bool asleep = false;
bool have_old = false;  // the controller's old plane / RAM holds the shown frame
bool grey_shown = false;  // a RefreshGrey picture is on the glass (its registers set)
uint8_t probe_flg[2] = {};
uint8_t probe_ver[2][kVersionSize] = {};
int screen_type = -1;  // stock NVS hw_calib/screenType, -1 unknown
uint32_t refreshes = 0;
uint32_t last_ms = 0;
Mode last_mode = Mode::kFull;
bool powered = false;  // UC81xx: the booster on (Refresh stay_on), until PowerOff
// The last B/W refresh by step (ms): new plane, power on, refresh, old plane, power off.
uint32_t step_ms[5] = {};
int64_t step_at = 0;
void Step(int i) {
    const int64_t now = esp_timer_get_time();
    step_ms[i] = static_cast<uint32_t>((now - step_at) / 1000);
    step_at = now;
}
bool last_grey = false;

spi_device_handle_t spi = nullptr;
uint8_t *bounce = nullptr;        // DMA-capable, kChunkRows rows
uint8_t *shown = nullptr;         // PSRAM: the frame on the glass (SSD1677 fast)
StaticSemaphore_t mutex_storage;
SemaphoreHandle_t mutex = nullptr;

void Level(gpio_num_t pin, int level) { gpio_set_level(pin, level); }

void Output(gpio_num_t pin, int level) {
    gpio_reset_pin(pin);
    gpio_set_direction(pin, GPIO_MODE_OUTPUT);
    gpio_set_level(pin, level);
}

// --- Detection: bit-banged half-duplex reads on the data line (~500 kHz) ---

void BangByte(uint8_t b) {
    for (int i = 0; i < 8; ++i) {
        Level(pins.mosi, (b & 0x80) != 0);
        esp_rom_delay_us(1);
        Level(pins.sclk, 1);
        esp_rom_delay_us(1);
        Level(pins.sclk, 0);
        b = static_cast<uint8_t>(b << 1);
    }
}

uint8_t BangRead() {
    uint8_t b = 0;
    for (int i = 0; i < 8; ++i) {
        esp_rom_delay_us(1);  // the controller shifts on the falling edge
        b = static_cast<uint8_t>((b << 1) | (gpio_get_level(pins.mosi) != 0 ? 1 : 0));
        Level(pins.sclk, 1);
        esp_rom_delay_us(1);
        Level(pins.sclk, 0);
    }
    return b;
}

void BangCommandRead(uint8_t cmd, uint8_t *out, size_t len) {
    gpio_set_direction(pins.mosi, GPIO_MODE_OUTPUT);
    Level(pins.dc, 0);
    Level(pins.cs, 0);
    esp_rom_delay_us(1);
    BangByte(cmd);
    Level(pins.dc, 1);
    gpio_set_direction(pins.mosi, GPIO_MODE_INPUT);
    gpio_set_pull_mode(pins.mosi, GPIO_PULLUP_ONLY);
    esp_rom_delay_us(1);
    for (size_t i = 0; i < len; ++i) {
        out[i] = BangRead();
    }
    Level(pins.cs, 1);
    gpio_set_pull_mode(pins.mosi, GPIO_FLOATING);
    gpio_set_direction(pins.mosi, GPIO_MODE_OUTPUT);
}

void ProbePass(int pass, uint32_t rst_low_ms) {
    Level(pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(10));
    Level(pins.rst, 0);
    esp_rom_delay_us(rst_low_ms * 1000);
    Level(pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(30));
    BangCommandRead(kCmdUcFlags, &probe_flg[pass], 1);
    BangCommandRead(kCmdUcVersion, probe_ver[pass], kVersionSize);
}

Controller Probe() {
    Output(pins.cs, 1);
    Output(pins.sclk, 0);
    Output(pins.dc, 0);
    Output(pins.mosi, 0);
    Output(pins.rst, 1);
    gpio_reset_pin(pins.busy);
    gpio_set_direction(pins.busy, GPIO_MODE_INPUT);
    ProbePass(0, 1);
    // The vendor's ID read holds reset 50 ms; only worth it on a likely UC81xx.
    ProbePass(1, LooksUc81xx(probe_flg[0], probe_ver[0]) ? 50 : 1);
    return Detect(probe_flg[0], probe_ver[0], probe_flg[1], probe_ver[1]);
}

int ReadScreenType() {
    nvs_handle_t nvs;
    if (nvs_open("hw_calib", NVS_READONLY, &nvs) != ESP_OK) {
        return -1;
    }
    uint8_t v = 0;
    const esp_err_t err = nvs_get_u8(nvs, "screenType", &v);
    nvs_close(nvs);
    return err == ESP_OK ? v : -1;
}

// --- SPI: CS and DC by hand, one CS pulse per command and per data block ---

esp_err_t Transmit(const uint8_t *data, size_t len) {
    spi_transaction_t t = {};
    t.length = len * 8;
    t.tx_buffer = data;
    return spi_device_polling_transmit(spi, &t);
}

esp_err_t Command(uint8_t cmd) {
    Level(pins.dc, 0);
    Level(pins.cs, 0);
    bounce[0] = cmd;
    const esp_err_t err = Transmit(bounce, 1);
    Level(pins.cs, 1);
    Level(pins.dc, 1);
    return err;
}

esp_err_t Send(const Cmd &c) {
    esp_err_t err = Command(c.cmd);
    if (err == ESP_OK && c.len > 0) {
        memcpy(bounce, c.data, c.len);
        Level(pins.cs, 0);
        err = Transmit(bounce, c.len);
        Level(pins.cs, 1);
    }
    return err;
}

template <size_t N>
esp_err_t SendAll(const Cmd (&list)[N]) {
    for (const Cmd &c : list) {
        const esp_err_t err = Send(c);
        if (err != ESP_OK) {
            return err;
        }
    }
    return ESP_OK;
}

esp_err_t SendBytes(uint8_t cmd, const uint8_t *data, size_t len) {
    esp_err_t err = Command(cmd);
    if (err == ESP_OK) {
        memcpy(bounce, data, len);
        Level(pins.cs, 0);
        err = Transmit(bounce, len);
        Level(pins.cs, 1);
    }
    return err;
}

// A plane after `cmd`: RamRows rows mapped by FrameRowFor, padding white;
// `frame` nullptr: every row `fill`; `invert`: the frame's rows inverted.
esp_err_t Plane(uint8_t cmd, const uint8_t *frame, uint8_t fill = 0xFF, bool invert = false) {
    esp_err_t err = Command(cmd);
    const uint16_t rows = RamRows(controller);
    Level(pins.cs, 0);
    for (uint16_t first = 0; first < rows && err == ESP_OK; first += kChunkRows) {
        const uint16_t n = static_cast<uint16_t>(rows - first < kChunkRows ? rows - first : kChunkRows);
        for (uint16_t r = 0; r < n; ++r) {
            uint8_t *dst = bounce + r * kRowBytes;
            const int src = FrameRowFor(controller, static_cast<uint16_t>(first + r));
            if (frame != nullptr && src != kPadRow) {
                memcpy(dst, frame + static_cast<size_t>(src) * kRowBytes, kRowBytes);
                for (size_t i = 0; invert && i < kRowBytes; ++i) {
                    dst[i] = static_cast<uint8_t>(~dst[i]);
                }
            } else {
                memset(dst, frame != nullptr ? 0xFF : fill, kRowBytes);
            }
        }
        err = Transmit(bounce, n * kRowBytes);
    }
    Level(pins.cs, 1);
    return err;
}

esp_err_t WaitIdle(uint32_t timeout_ms = kBusyTimeoutMs) {
    const int64_t until = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (Busy(controller, gpio_get_level(pins.busy))) {
        if (esp_timer_get_time() > until) {
            return ESP_ERR_TIMEOUT;
        }
        vTaskDelay(1);
    }
    return ESP_OK;
}

// A refresh may take a moment to raise BUSY: wait up to 50 ms for it to
// start, then for it to end.
esp_err_t WaitRefresh() {
    for (int i = 0; i < 50 && !Busy(controller, gpio_get_level(pins.busy)); ++i) {
        esp_rom_delay_us(1000);
    }
    return WaitIdle();
}

void Reset() {
    Level(pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(kResetStepMs));
    Level(pins.rst, 0);
    vTaskDelay(pdMS_TO_TICKS(kResetStepMs));
    Level(pins.rst, 1);
    vTaskDelay(pdMS_TO_TICKS(kResetStepMs + ResetSettleMs(controller)));
}

esp_err_t InitController() {
    Reset();
    esp_err_t err = ESP_OK;
    if (controller == Controller::kSsd1677) {
        err = Command(kSsdSwReset);
        vTaskDelay(pdMS_TO_TICKS(10));  // fixed, then BUSY (X4 Pro needs both)
        if (err == ESP_OK) err = WaitIdle();
        if (err == ESP_OK) err = SendAll(kSsdInit);
    } else if (controller == Controller::kUc8179) {
        err = SendAll(kUc8179Init);
    } else {
        err = SendAll(kUc8279Init);
    }
    have_old = false;
    asleep = false;
    powered = false;  // the reset powered it down
    return err;
}

esp_err_t PowerOffUc() {
    esp_err_t err = ESP_OK;
    if (powered) {
        err = Command(kUcPowerOff);
        if (err == ESP_OK) err = WaitIdle();
        powered = false;
    }
    return err;
}

esp_err_t RefreshSsd(const uint8_t *frame, Mode mode) {
    esp_err_t err = SendAll(kSsdCursor);
    if (err == ESP_OK) err = Plane(kSsdNew, frame);
    if (err == ESP_OK) err = SendAll(kSsdCursor);
    // Old RAM: the frame for a full refresh, the one on the glass for a fast one.
    if (err == ESP_OK) err = Plane(kSsdOld, mode == Mode::kFull ? frame : shown);
    if (err == ESP_OK) err = mode == Mode::kFull ? SendAll(kSsdFull) : SendAll(kSsdFast);
    if (err == ESP_OK) err = Command(kSsdActivate);
    if (err == ESP_OK) err = WaitRefresh();
    // 0xF7 powers down by itself, 0xFC does not.
    if (err == ESP_OK && mode == Mode::kFast) {
        err = SendAll(kSsdPowerOff);
        if (err == ESP_OK) err = Command(kSsdActivate);
        if (err == ESP_OK) err = WaitRefresh();
    }
    return err;
}

esp_err_t RefreshUc(const uint8_t *frame, Mode mode, bool stay_on) {
    const bool uc8279 = controller == Controller::kUc8279;
    step_at = esp_timer_get_time();
    esp_err_t err = Plane(kUcNew, frame);
    if (err == ESP_OK && mode == Mode::kFull) err = Plane(kUcOld, nullptr, 0xFF);
    if (err == ESP_OK) {
        if (uc8279) {
            err = mode == Mode::kFull ? SendAll(kUc8279Full) : SendAll(kUc8279Fast);
        } else {
            err = mode == Mode::kFull ? SendAll(kUc8179Full) : SendAll(kUc8179Fast);
        }
    }
    Step(0);
    if (err == ESP_OK && !powered) {
        err = Command(kUcPowerOn);
        if (err == ESP_OK) err = WaitIdle();
        powered = err == ESP_OK;
    }
    Step(1);
    if (err == ESP_OK && mode == Mode::kFast) {
        err = Command(kUcPartialIn);
        if (err == ESP_OK && uc8279) err = Send(kUc8279Window);
    }
    if (err == ESP_OK && uc8279) err = Send(kUc8279Psr);  // after power on, or it does not latch
    if (err == ESP_OK) err = Command(kUcRefresh);
    if (err == ESP_OK) err = WaitRefresh();
    if (err == ESP_OK && mode == Mode::kFast) err = Command(kUcPartialOut);
    if (err == ESP_OK && !uc8279) err = Send(kUc8179Idle);
    Step(2);
    // The old plane now matches the glass: the next fast refresh diffs against it.
    if (err == ESP_OK) err = Plane(kUcOld, frame);
    Step(3);
    if (err == ESP_OK && !stay_on) err = PowerOffUc();
    Step(4);
    return err;
}

// After a grey picture: every pixel driven to the frame (old = its inverse),
// fast, before the full refresh. FreeInk's displayStart after Direct.
esp_err_t ClearGreyUc(const uint8_t *frame) {
    esp_err_t err = Plane(kUcOld, frame, 0xFF, true);
    if (err == ESP_OK) err = Plane(kUcNew, frame);
    if (err == ESP_OK) err = SendAll(kUc8179Fast);
    if (err == ESP_OK) err = Command(kUcPowerOn);
    if (err == ESP_OK) err = WaitIdle();
    powered = err == ESP_OK;
    if (err == ESP_OK) err = Command(kUcPartialIn);
    if (err == ESP_OK) err = Command(kUcRefresh);
    if (err == ESP_OK) err = WaitRefresh();
    if (err == ESP_OK) err = Command(kUcPartialOut);
    return err;
}

esp_err_t RefreshGreyUc(const uint8_t *low, const uint8_t *high) {
    esp_err_t err = InitController();  // from the B/W registers' defaults
    if (err == ESP_OK) err = SendAll(kUc8179GreyPower);
    for (uint8_t t = 0; t < 5 && err == ESP_OK; ++t) {
        uint8_t lut[kGreyLutBytes] = {};
        memcpy(lut, kUc8179GreyLuts[t], kGreyLutUsed);
        err = SendBytes(static_cast<uint8_t>(kGreyLutFirst + t), lut, sizeof lut);
    }
    if (err == ESP_OK) err = Plane(kUcOld, low);
    if (err == ESP_OK) err = Plane(kUcNew, high);
    if (err == ESP_OK) err = Command(kUcPowerOn);
    if (err == ESP_OK) err = WaitIdle();
    if (err == ESP_OK) err = Command(kUcRefresh);
    if (err == ESP_OK) err = WaitRefresh();
    if (err == ESP_OK) err = Command(kUcPowerOff);
    if (err == ESP_OK) err = WaitIdle();
    return err;
}

}  // namespace

esp_err_t Init() {
    const boards::Board &b = boards::kBoard;
    if (b.display == boards::DisplayType::kNone || ready) {
        return ready ? ESP_OK : ESP_ERR_NOT_SUPPORTED;
    }
    const boards::DisplayPins &p = b.display_pins;
    pins = {static_cast<gpio_num_t>(p.mosi), static_cast<gpio_num_t>(p.sclk),
            static_cast<gpio_num_t>(p.cs),   static_cast<gpio_num_t>(p.dc),
            static_cast<gpio_num_t>(p.reset), static_cast<gpio_num_t>(p.busy)};
    screen_type = ReadScreenType();
    controller = Probe();

    bounce = static_cast<uint8_t *>(heap_caps_malloc(kChunkRows * kRowBytes, MALLOC_CAP_DMA));
    shown = static_cast<uint8_t *>(heap_caps_malloc(kFrameBytes, MALLOC_CAP_SPIRAM));
    if (bounce == nullptr || shown == nullptr) {
        return ESP_ERR_NO_MEM;
    }
    memset(shown, 0xFF, kFrameBytes);

    spi_bus_config_t bus = {};
    bus.mosi_io_num = pins.mosi;
    bus.miso_io_num = -1;
    bus.sclk_io_num = pins.sclk;
    bus.quadwp_io_num = -1;
    bus.quadhd_io_num = -1;
    bus.max_transfer_sz = kChunkRows * kRowBytes;
    esp_err_t err = spi_bus_initialize(kHost, &bus, SPI_DMA_CH_AUTO);
    if (err == ESP_OK) {
        spi_device_interface_config_t dev = {};
        dev.mode = 0;
        dev.clock_speed_hz = kSpiHz;
        dev.spics_io_num = -1;  // CS by hand: one pulse per command / data block
        dev.queue_size = 1;
        err = spi_bus_add_device(kHost, &dev, &spi);
    }
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "spi: %s", esp_err_to_name(err));
        return err;
    }
    mutex = xSemaphoreCreateMutexStatic(&mutex_storage);
    err = InitController();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s init: %s", Name(controller), esp_err_to_name(err));
        return err;
    }
    ready = true;
    ESP_LOGI(TAG, "%s (FLG 0x%02x, VER %02x %02x %02x %02x %02x), stock screenType %d", Name(controller),
             probe_flg[0], probe_ver[0][0], probe_ver[0][1], probe_ver[0][2], probe_ver[0][3],
             probe_ver[0][4], screen_type);
    return ESP_OK;
}

bool Ready() { return ready; }

Controller Detected() { return controller; }

esp_err_t Refresh(const uint8_t *frame, Mode mode, bool stay_on) {
    if (!ready || frame == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    // A grey picture left its registers set: B/W wants them back to defaults.
    esp_err_t err = asleep || grey_shown ? InitController() : ESP_OK;
    if (!have_old) {
        mode = Mode::kFull;  // nothing known on the glass to diff against
    }
    const int64_t start = esp_timer_get_time();
    if (err == ESP_OK && grey_shown) {
        err = ClearGreyUc(frame);
        grey_shown = false;
    }
    if (err == ESP_OK) {
        err = controller == Controller::kSsd1677 ? RefreshSsd(frame, mode) : RefreshUc(frame, mode, stay_on);
    }
    last_ms = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    last_mode = mode;
    last_grey = false;
    if (err == ESP_OK) {
        memcpy(shown, frame, kFrameBytes);
        have_old = true;
        ++refreshes;
    } else {
        have_old = false;
        ESP_LOGE(TAG, "refresh: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t RefreshGrey(const uint8_t *low, const uint8_t *high) {
    if (!ready || low == nullptr || high == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    if (!HasGrey(controller)) {
        return ESP_ERR_NOT_SUPPORTED;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const int64_t start = esp_timer_get_time();
    PowerOffUc();  // its init resets the controller: not with the booster on
    const esp_err_t err = RefreshGreyUc(low, high);
    last_ms = static_cast<uint32_t>((esp_timer_get_time() - start) / 1000);
    last_grey = true;
    have_old = false;
    grey_shown = true;  // even after a failure: the registers may be grey ones
    if (err == ESP_OK) {
        ++refreshes;
    } else {
        ESP_LOGE(TAG, "grey refresh: %s", esp_err_to_name(err));
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t Sleep() {
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (!asleep) {
        if (controller == Controller::kSsd1677) {
            err = SendAll(kSsdPowerOff);
            if (err == ESP_OK) err = Command(kSsdActivate);
            if (err == ESP_OK) err = WaitRefresh();
            if (err == ESP_OK) err = Send(kSsdDeepSleep);
        } else {
            err = PowerOffUc();
            if (err == ESP_OK) err = Send(kUcDeepSleep);
        }
        asleep = err == ESP_OK;
    }
    xSemaphoreGive(mutex);
    return err;
}

esp_err_t PowerOff() {
    if (!ready) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(mutex, portMAX_DELAY);
    const esp_err_t err = controller == Controller::kSsd1677 ? ESP_OK : PowerOffUc();
    xSemaphoreGive(mutex);
    return err;
}

void PrintInfo() {
    if (boards::kBoard.display == boards::DisplayType::kNone) {
        printf("epd        none on this board\n");
        return;
    }
    printf("epd        %s, %ux%u, %s%s%s\n", Name(controller), kWidth, kHeight,
           ready ? "ready" : "not running", asleep ? ", asleep" : "",
           powered ? ", powered" : "");
    for (int pass = 0; pass < 2; ++pass) {
        printf("probe %d    FLG 0x%02x  VER %02x %02x %02x %02x %02x\n", pass + 1, probe_flg[pass],
               probe_ver[pass][0], probe_ver[pass][1], probe_ver[pass][2], probe_ver[pass][3],
               probe_ver[pass][4]);
    }
    if (screen_type >= 0) {
        printf("stock      screenType %d (%s)\n", screen_type,
               Name(FromScreenType(static_cast<uint8_t>(screen_type))));
    }
    printf("refreshes  %" PRIu32 ", last %s in %" PRIu32 " ms\n", refreshes,
           last_grey ? "grey" : last_mode == Mode::kFull ? "full" : "fast", last_ms);
    printf("steps      new plane %" PRIu32 ", power on %" PRIu32 ", refresh %" PRIu32
           ", old plane %" PRIu32 ", power off %" PRIu32 " ms\n",
           step_ms[0], step_ms[1], step_ms[2], step_ms[3], step_ms[4]);
}

}  // namespace epd
