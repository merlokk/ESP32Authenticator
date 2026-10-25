#include "usb.h"

#include <cerrno>
#include <cinttypes>
#include <cstdio>
#include <cstring>

#include <atomic>

#include <fcntl.h>
#include <sys/stat.h>

#include "driver/usb_serial_jtag.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_newlib.h"
#include "esp_rom_sys.h"
#include "esp_sleep.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_vfs.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "hal/usb_serial_jtag_ll.h"
#include "hal/usb_wrap_ll.h"
#include "tinyusb.h"
#include "tinyusb_cdc_acm.h"
#include "tinyusb_default_config.h"
#include "usb_fido.h"

namespace usb {

namespace {

constexpr const char *TAG = "usb";

// The stdio device. The VFS matches by prefix: no other device's name starts
// with it, nor it with another's.
constexpr const char *kDevicePath = "/dev/authusb";

// No D+ pull-up this long and the host has seen the device go (a hub needs
// microseconds; hosts debounce for tens of ms).
constexpr TickType_t kDetachTicks = pdMS_TO_TICKS(100);
// A read waits in slices: a switch never waits long for a blocked reader.
constexpr TickType_t kReadSlice = pdMS_TO_TICKS(50);
// Output waits this long for room while the host reads it; then it is dropped.
constexpr int64_t kTxWaitUs = 50 * 1000;
constexpr TickType_t kTxWaitTicks = pdMS_TO_TICKS(50);
// A writer waits this long for a switch to end; then its output is dropped.
constexpr TickType_t kTxLockTicks = pdMS_TO_TICKS(500);
// TinyUSB's task: it must never wait for its own flush.
constexpr const char *kUsbTaskName = "TinyUSB";

enum class Port : uint8_t {
    kNone,  // switching, or nothing came up
    kJtag,  // the Serial/JTAG driver
    kCdc,   // TinyUSB's CDC-ACM port
};

Mode mode = Mode::kJtag;
bool started = false;
volatile Port port = Port::kNone;
SemaphoreHandle_t switch_lock = nullptr;  // one SetMode at a time
SemaphoreHandle_t rx_lock = nullptr;      // a read against the port; held by a switch
SemaphoreHandle_t tx_lock = nullptr;      // a write against the port; held by a switch
SemaphoreHandle_t cdc_rx = nullptr;       // given by TinyUSB when data came
// While a switch runs, output is dropped at once: a writer waiting for it
// (TinyUSB's own task logging while it starts) would hold the switch up.
std::atomic<bool> switching{false};
int64_t last_tx_us = 0;                   // the port last took output
char tx_buf[128];                         // under tx_lock

// The last switch that failed, for `usb` (its log line may have been lost
// with the port).
struct Failure {
    Mode to;
    const char *step;
    esp_err_t err;
    int64_t at_us;
};
Failure failure = {Mode::kJtag, nullptr, ESP_OK, 0};

esp_err_t Note(Mode to, const char *step, esp_err_t err) {
    if (err != ESP_OK) {
        failure = {to, step, err, esp_timer_get_time()};
    }
    return err;
}

// The stdio device's open files: picolibc opens one for stdin, one for stdout.
constexpr int kMaxFds = 4;
bool fd_used[kMaxFds];
int fd_flags[kMaxFds];

// --- Descriptors (TinyUSB keeps the pointers: static)

enum : uint8_t { kItfCdc, kItfCdcData, kItfHid };
enum : uint8_t { kStrLang, kStrMaker, kStrProduct, kStrSerial, kStrCdc, kStrHid, kStrCount };
constexpr uint8_t kEpCdcNotify = 0x81;
constexpr uint8_t kEpCdcOut = 0x02;
constexpr uint8_t kEpCdcIn = 0x82;
constexpr uint8_t kEpHidOut = 0x03;
constexpr uint8_t kEpHidIn = 0x83;
constexpr uint8_t kHidIntervalMs = 5;
constexpr uint16_t kPowerMa = 500;  // the charger draws it anyway

const uint8_t kFidoReport[] = {TUD_HID_REPORT_DESC_FIDO_U2F(CFG_TUD_HID_EP_BUFSIZE)};

const uint8_t kConfigSerial[] = {
    TUD_CONFIG_DESCRIPTOR(1, 2, 0, TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN, 0, kPowerMa),
    TUD_CDC_DESCRIPTOR(kItfCdc, kStrCdc, kEpCdcNotify, 8, kEpCdcOut, kEpCdcIn, 64),
};

const uint8_t kConfigSerialFido[] = {
    TUD_CONFIG_DESCRIPTOR(1, 3, 0,
                          TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_HID_INOUT_DESC_LEN, 0,
                          kPowerMa),
    TUD_CDC_DESCRIPTOR(kItfCdc, kStrCdc, kEpCdcNotify, 8, kEpCdcOut, kEpCdcIn, 64),
    TUD_HID_INOUT_DESCRIPTOR(kItfHid, kStrHid, HID_ITF_PROTOCOL_NONE, sizeof kFidoReport,
                             kEpHidOut, kEpHidIn, CFG_TUD_HID_EP_BUFSIZE, kHidIntervalMs),
};

char serial[13];  // the base MAC in hex, as the BLE Device Information's
const char *strings[kStrCount] = {
    "\x09\x04",  // English (US)
    "ESP32 Auth",
    "ESP32 Authenticator",
    serial,
    "ESP32 Auth console",
    "ESP32 Auth FIDO2",
};
tusb_desc_device_t device;

// --- Serial/JTAG

// The pulls back to the Serial/JTAG's own. The override's values stay in its
// registers after it is off (and through a software reset): D+'s pull-up
// written back to its reset value, 1, or the host never sees the port again.
void IRAM_ATTR ReleasePulls() {
    const usb_serial_jtag_pull_override_vals_t own = {
        .dp_pu = true, .dm_pu = false, .dp_pd = false, .dm_pd = false};
    usb_serial_jtag_ll_phy_enable_pull_override(&own);
    usb_serial_jtag_ll_phy_disable_pull_override();
}

esp_err_t StartJtag() {
    usb_serial_jtag_driver_config_t cfg = {};
    cfg.tx_buffer_size = 512;
    cfg.rx_buffer_size = 512;
    const esp_err_t err = Note(Mode::kJtag, "Serial/JTAG driver",
                               usb_serial_jtag_driver_install(&cfg));  // the PHY back to it
    ReleasePulls();  // D+ pulled up: the host sees it arrive
    if (err == ESP_OK) {
        port = Port::kJtag;
    }
    return err;
}

void StopJtag() {
    if (usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_wait_tx_done(kTxWaitTicks);
        usb_serial_jtag_driver_uninstall();
    }
    const usb_serial_jtag_pull_override_vals_t off = {
        .dp_pu = false, .dm_pu = false, .dp_pd = false, .dm_pd = false};
    usb_serial_jtag_ll_phy_enable_pull_override(&off);
    vTaskDelay(kDetachTicks);
}

// --- TinyUSB

void OnCdcRx(int, cdcacm_event_t *) { xSemaphoreGive(cdc_rx); }

esp_err_t StartOtg(Mode m) {
    device = {
        .bLength = sizeof(tusb_desc_device_t),
        .bDescriptorType = TUSB_DESC_DEVICE,
        .bcdUSB = 0x0200,
        // Interface association: the CDC's two interfaces are one function.
        .bDeviceClass = TUSB_CLASS_MISC,
        .bDeviceSubClass = MISC_SUBCLASS_COMMON,
        .bDeviceProtocol = MISC_PROTOCOL_IAD,
        .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
        .idVendor = kVendorId,
        .idProduct = ProductId(m),
        .bcdDevice = 0x0100,
        .iManufacturer = kStrMaker,
        .iProduct = kStrProduct,
        .iSerialNumber = kStrSerial,
        .bNumConfigurations = 1,
    };
    tinyusb_config_t cfg = {};
    cfg.port = TINYUSB_PORT_FULL_SPEED_0;
    cfg.phy.skip_setup = false;
    cfg.phy.self_powered = false;
    cfg.phy.vbus_monitor_io = -1;
    cfg.task.size = TINYUSB_DEFAULT_TASK_SIZE;
    cfg.task.priority = TINYUSB_DEFAULT_TASK_PRIO;
    cfg.task.xCoreID = TINYUSB_DEFAULT_TASK_AFFINITY;
    cfg.descriptor.device = &device;
    cfg.descriptor.string = strings;
    cfg.descriptor.string_count = kStrCount;
    cfg.descriptor.full_speed_config = HasFido(m) ? kConfigSerialFido : kConfigSerial;
    esp_err_t err = Note(m, "TinyUSB", tinyusb_driver_install(&cfg));  // takes the PHY
    if (err != ESP_OK) {
        return err;
    }
    tinyusb_config_cdcacm_t acm = {};
    acm.cdc_port = TINYUSB_CDC_ACM_0;
    acm.callback_rx = OnCdcRx;
    err = Note(m, "CDC-ACM", tinyusb_cdcacm_init(&acm));
    if (err != ESP_OK) {
        tinyusb_driver_uninstall();
        return err;
    }
    if (HasFido(m)) {
        fido::Start();
    }
    port = Port::kCdc;
    return ESP_OK;
}

void StopOtg(bool had_fido) {
    if (had_fido) {
        fido::Stop();
    }
    // What is still queued goes out first (a terminal reading).
    const int64_t until = esp_timer_get_time() + kTxWaitUs;
    while (tud_cdc_n_connected(0) && tud_cdc_n_write_available(0) < CFG_TUD_CDC_TX_BUFSIZE &&
           esp_timer_get_time() < until) {
        tud_cdc_n_write_flush(0);
        vTaskDelay(1);
    }
    tud_disconnect();  // D+ released: the host sees the device go
    vTaskDelay(kDetachTicks);
    tinyusb_cdcacm_deinit(TINYUSB_CDC_ACM_0);
    tinyusb_driver_uninstall();
}

// The internal PHY back to the Serial/JTAG, its pull-up on. A software reset
// keeps the USB peripherals as the run left them: an OTG controller still
// connected holds the pads and the Serial/JTAG never shows (not even to the
// ROM), so it is reset, its clock off. Register writes only: also the deep
// sleep's hook, and at boot.
void IRAM_ATTR GiveBackPhy() {
    _usb_wrap_ll_enable_bus_clock(true);
    _usb_wrap_ll_reset_register();
    _usb_wrap_ll_enable_bus_clock(false);
    usb_serial_jtag_ll_phy_enable_external(false);
    usb_serial_jtag_ll_phy_enable_pad(true);
    ReleasePulls();
}

// esp_restart: the ROM, the bootloader and the next boot's first lines on the
// Serial/JTAG (and its download mode reachable), as after a power-on.
void OnRestart() {
    if (port == Port::kCdc) {
        tud_disconnect();
        esp_rom_delay_us(100 * 1000);  // the host sees the device go
    }
    GiveBackPhy();
}

void IRAM_ATTR OnDeepSleep() { GiveBackPhy(); }

// Under rx_lock and tx_lock.
esp_err_t Apply(Mode next) {
    port = Port::kNone;
    if (started) {
        if (OnOtg(mode)) {
            StopOtg(HasFido(mode));
        } else {
            StopJtag();
        }
    } else {
        GiveBackPhy();  // boot: whatever the last run left on the pads gone
        if (OnOtg(next)) {
            StopJtag();  // the ROM's Serial/JTAG leaves first
        }
    }
    esp_err_t err = OnOtg(next) ? StartOtg(next) : StartJtag();
    if (err != ESP_OK && OnOtg(next)) {
        StartJtag();  // the console stays reachable
        next = Mode::kJtag;
    }
    mode = next;
    started = true;
    return err;
}

// --- Console I/O

bool InUsbTask() { return strcmp(pcTaskGetName(nullptr), kUsbTaskName) == 0; }

// Up to `size` bytes, waiting up to `wait` (<= a slice).
int ReadSlice(void *buf, size_t size, TickType_t wait) {
    if (xSemaphoreTake(rx_lock, wait) != pdTRUE) {
        return 0;
    }
    int n = 0;
    switch (port) {
        case Port::kJtag:
            n = usb_serial_jtag_read_bytes(buf, size, wait);
            break;
        case Port::kCdc: {
            size_t got = 0;
            tinyusb_cdcacm_read(TINYUSB_CDC_ACM_0, static_cast<uint8_t *>(buf), size, &got);
            n = static_cast<int>(got);
            if (n == 0) {
                xSemaphoreGive(rx_lock);  // a switch may go on while this waits
                xSemaphoreTake(cdc_rx, wait);
                return 0;
            }
            break;
        }
        case Port::kNone:
            xSemaphoreGive(rx_lock);
            vTaskDelay(wait);
            return 0;
    }
    xSemaphoreGive(rx_lock);
    return n;
}

// False: the host is not reading; the rest is dropped.
bool SendJtag(const char *buf, size_t n) {
    if (!usb_serial_jtag_is_connected()) {
        return false;
    }
    // Waits only while the host keeps reading: an open port nobody reads would
    // hold every line up.
    const int64_t now = esp_timer_get_time();
    const TickType_t wait = now - last_tx_us < kTxWaitUs ? kTxWaitTicks : 0;
    if (usb_serial_jtag_write_bytes(buf, n, wait) <= 0) {
        return false;
    }
    last_tx_us = now;
    return true;
}

// Without DTR TinyUSB's FIFO overwrites its oldest bytes (never full): a
// terminal that does not raise DTR (utils/ keep it low: on the Serial/JTAG it
// resets the chip) still gets the latest output. With DTR it waits only
// while the host keeps reading, as SendJtag.
bool SendCdc(const char *buf, size_t n) {
    if (!tud_mounted()) {
        return false;
    }
    const bool usb_task = InUsbTask();
    size_t done = 0;
    while (done < n) {
        const uint32_t room = tud_cdc_n_write_available(0);
        const uint32_t k =
            room > 0 ? tud_cdc_n_write(0, buf + done, room < n - done ? room : n - done) : 0;
        tud_cdc_n_write_flush(0);
        const int64_t now = esp_timer_get_time();
        if (k > 0) {
            done += k;
            last_tx_us = now;
        } else if (usb_task || now - last_tx_us > kTxWaitUs) {
            return false;
        } else {
            vTaskDelay(1);
        }
    }
    return true;
}

void Write(const char *data, size_t size) {
    if (switching || xSemaphoreTake(tx_lock, kTxLockTicks) != pdTRUE) {
        return;
    }
    size_t i = 0;
    while (i < size) {
        size_t used = 0;
        const size_t n = ExpandNewlines(data + i, size - i, tx_buf, sizeof tx_buf, &used);
        i += used;
        const bool sent = port == Port::kJtag  ? SendJtag(tx_buf, n)
                          : port == Port::kCdc ? SendCdc(tx_buf, n)
                                               : false;
        if (!sent) {
            break;
        }
    }
    xSemaphoreGive(tx_lock);
}

// --- The stdio device

int VfsOpen(void *, const char *, int flags, int) {
    for (int fd = 0; fd < kMaxFds; ++fd) {
        if (!fd_used[fd]) {
            fd_used[fd] = true;
            fd_flags[fd] = flags;
            return fd;
        }
    }
    errno = ENFILE;
    return -1;
}

int VfsClose(void *, int fd) {
    if (fd < 0 || fd >= kMaxFds || !fd_used[fd]) {
        errno = EBADF;
        return -1;
    }
    fd_used[fd] = false;
    return 0;
}

ssize_t VfsWrite(void *, int, const void *data, size_t size) {
    Write(static_cast<const char *>(data), size);
    return static_cast<ssize_t>(size);  // dropped or not: never an error to the log
}

ssize_t VfsRead(void *, int fd, void *dst, size_t size) {
    const bool nonblocking = fd >= 0 && fd < kMaxFds && (fd_flags[fd] & O_NONBLOCK) != 0;
    const int n = Read(dst, size, nonblocking ? 0 : portMAX_DELAY);
    if (n == 0 && nonblocking && size > 0) {
        errno = EAGAIN;
        return -1;
    }
    return n;
}

int VfsFstat(void *, int, struct stat *st) {
    memset(st, 0, sizeof *st);
    st->st_mode = S_IFCHR;
    return 0;
}

int VfsFcntl(void *, int fd, int cmd, int arg) {
    if (fd < 0 || fd >= kMaxFds || !fd_used[fd]) {
        errno = EBADF;
        return -1;
    }
    if (cmd == F_GETFL) {
        return fd_flags[fd];
    }
    if (cmd == F_SETFL) {
        fd_flags[fd] = arg;
        return 0;
    }
    errno = ENOSYS;
    return -1;
}

int VfsFsync(void *, int) {
    if (port == Port::kCdc && !switching && xSemaphoreTake(tx_lock, kTxLockTicks) == pdTRUE) {
        if (port == Port::kCdc) {
            tud_cdc_n_write_flush(0);
        }
        xSemaphoreGive(tx_lock);
    }
    return 0;
}

const esp_vfs_fs_ops_t kVfs = {
    .write_p = VfsWrite,
    .read_p = VfsRead,
    .open_p = VfsOpen,
    .close_p = VfsClose,
    .fstat_p = VfsFstat,
    .fcntl_p = VfsFcntl,
    .fsync_p = VfsFsync,
};

}  // namespace

esp_err_t Init(Mode m) {
    if (switch_lock != nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    switch_lock = xSemaphoreCreateMutex();
    rx_lock = xSemaphoreCreateMutex();
    tx_lock = xSemaphoreCreateMutex();
    cdc_rx = xSemaphoreCreateBinary();
    uint8_t mac[6] = {};
    esp_efuse_mac_get_default(mac);
    snprintf(serial, sizeof serial, "%02X%02X%02X%02X%02X%02X", mac[0], mac[1], mac[2], mac[3],
             mac[4], mac[5]);
    esp_err_t err = esp_vfs_register_fs(kDevicePath, &kVfs,
                                        ESP_VFS_FLAG_STATIC | ESP_VFS_FLAG_CONTEXT_PTR, nullptr);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s", kDevicePath, esp_err_to_name(err));
        return err;
    }
    esp_register_shutdown_handler(OnRestart);
    esp_deep_sleep_register_hook(OnDeepSleep);

    switching = true;
    xSemaphoreTake(rx_lock, portMAX_DELAY);
    xSemaphoreTake(tx_lock, portMAX_DELAY);
    err = Apply(m);
    xSemaphoreGive(tx_lock);
    xSemaphoreGive(rx_lock);
    switching = false;

    fflush(stdout);
    esp_libc_init_global_stdio(kDevicePath);  // stdin, stdout, stderr: ours from here
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "%s: %s; the port is %s", Name(m), esp_err_to_name(err), Name(mode));
    } else {
        ESP_LOGI(TAG, "%s", Name(mode));
    }
    return err;
}

esp_err_t SetMode(Mode next) {
    if (switch_lock == nullptr) {
        return ESP_ERR_INVALID_STATE;
    }
    xSemaphoreTake(switch_lock, portMAX_DELAY);
    esp_err_t err = ESP_OK;
    if (next != mode) {
        const Mode was = mode;
        fflush(stdout);
        switching = true;
        xSemaphoreTake(rx_lock, portMAX_DELAY);
        xSemaphoreTake(tx_lock, portMAX_DELAY);
        err = Apply(next);
        xSemaphoreGive(tx_lock);
        xSemaphoreGive(rx_lock);
        switching = false;
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s: %s; the port is %s", Name(next), esp_err_to_name(err), Name(mode));
        } else {
            ESP_LOGI(TAG, "%s -> %s", Name(was), Name(mode));
        }
    }
    xSemaphoreGive(switch_lock);
    return err;
}

Mode GetMode() { return mode; }

bool HostConnected() {
    switch (port) {
        case Port::kJtag: return usb_serial_jtag_is_connected();
        case Port::kCdc: return tud_mounted() && !tud_suspended();
        case Port::kNone: break;
    }
    return false;
}

bool PortOpen() {
    switch (port) {
        case Port::kJtag: return usb_serial_jtag_is_connected();
        case Port::kCdc: return tud_cdc_n_connected(0);
        case Port::kNone: break;
    }
    return false;
}

int Read(void *buf, size_t size, TickType_t timeout) {
    if (size == 0 || rx_lock == nullptr) {
        return 0;
    }
    TimeOut_t start;
    vTaskSetTimeOutState(&start);
    for (;;) {
        const int n = ReadSlice(buf, size, timeout < kReadSlice ? timeout : kReadSlice);
        if (n > 0) {
            CrToLf(static_cast<char *>(buf), static_cast<size_t>(n));
            return n;
        }
        if (timeout == 0 || xTaskCheckForTimeOut(&start, &timeout) != pdFALSE) {
            return 0;
        }
    }
}

void PrintInfo() {
    printf("mode       %s (%s)\n", Name(mode),
           OnOtg(mode) ? "TinyUSB on the OTG controller" : "the chip's USB Serial/JTAG");
    if (OnOtg(mode)) {
        printf("device     %04x:%04x, serial %s\n", kVendorId, ProductId(mode), serial);
    }
    printf("host       %s\n", HostConnected() ? "connected" : "none");
    if (OnOtg(mode)) {
        printf("port       %s\n", PortOpen() ? "open (DTR)" : "closed");
    }
    if (failure.err != ESP_OK) {
        printf("failed     %s, %s: %s (%lld s ago)\n", Name(failure.to), failure.step,
               esp_err_to_name(failure.err),
               static_cast<long long>((esp_timer_get_time() - failure.at_us) / 1000000));
    }
    if (HasFido(mode)) {
        uint32_t requests = 0;
        uint32_t errors = 0;
        uint32_t dropped = 0;
        fido::Stats(&requests, &errors, &dropped);
        printf("fido       %" PRIu32 " requests, %" PRIu32 " errors, %" PRIu32
               " reports dropped\n",
               requests, errors, dropped);
    }
}

}  // namespace usb

// --- TinyUSB's HID callbacks: the FIDO2 / U2F interface (serial+fido only).

extern "C" {

uint8_t const *tud_hid_descriptor_report_cb(uint8_t) { return usb::kFidoReport; }

uint16_t tud_hid_get_report_cb(uint8_t, uint8_t, hid_report_type_t, uint8_t *, uint16_t) {
    return 0;
}

void tud_hid_set_report_cb(uint8_t, uint8_t, hid_report_type_t type, uint8_t const *buffer,
                           uint16_t size) {
    if (type == HID_REPORT_TYPE_OUTPUT) {
        usb::fido::OnReport(buffer, size);
    }
}

void tud_hid_report_complete_cb(uint8_t, uint8_t const *, uint16_t) { usb::fido::OnSent(); }

}  // extern "C"
