#pragma once

// The Wi-Fi radio, and no decisions: join this network, be an access point,
// what the link is doing, what is on the air. Which network and when is
// `components/wifimgr` (ported from approver-esp32).
//
// - Credentials never go to the driver's own store (WIFI_STORAGE_RAM):
//   config.json is the only record.
// - Bring-up is in two halves: Init() once (netif, event loop, handlers);
//   the Wi-Fi stack itself (netifs + esp_wifi_init) is taken by the first
//   Start / Scan and given back by Stop().
// - The AP runs as APSTA: the station interface stays up (idle, or joining
//   when asked with `keep_ap`) so a scan works while the AP is up.
// - A disconnect this code asked for is not reported as a failure.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

namespace wifi {

constexpr size_t kSsidSize = 33;      // 32 bytes + NUL
constexpr size_t kPasswordSize = 65;  // 8..63 chars or 64 hex digits + NUL
constexpr size_t kHostnameSize = 33;
constexpr size_t kMaxScanResults = 16;
constexpr size_t kMinApPasswordLength = 8;

enum class Mode : uint8_t {
    kOff,
    kClient,  // station
    kAp,      // access point (station interface idle)
    kApSta,   // access point, and the station joining or joined
};

// The station link.
enum class Link : uint8_t {
    kIdle,
    kConnecting,
    kConnected,  // associated and holding an IPv4 address
    kFailed,     // latched until the next StartClient / StartAp
};

enum class Failure : uint8_t {
    kNone,
    kAuth,      // wrong password
    kNotFound,  // no such network on the air
    kOther,     // drop, timeout, anything else
};

// A fixed address for the network being joined, binary (esp_netif order:
// first octet in the low byte). The text lives in config.json; the manager
// converts. A zero DNS entry is not set.
struct StaticIp {
    bool enabled = false;
    uint32_t address = 0;
    uint32_t netmask = 0;
    uint32_t gateway = 0;
    uint32_t dns1 = 0;
    uint32_t dns2 = 0;
};

struct Status {
    Mode mode = Mode::kOff;
    Link link = Link::kIdle;
    Failure failure = Failure::kNone;
    uint8_t reason = 0;  // raw wifi_err_reason_t of the last failure

    char ssid[kSsidSize] = {};  // network being joined / joined
    int8_t rssi = 0;
    uint8_t channel = 0;  // tuned channel (the AP follows the station's)
    uint32_t ip = 0;      // station IPv4 (esp_ip4_addr_t order), 0 when none
    bool ip_is_static = false;

    char ap_ssid[kSsidSize] = {};
    uint32_t ap_ip = 0;
    uint8_t clients = 0;  // stations on our AP
    uint32_t changes = 0;  // bumped on every transition
};

struct ScanResult {
    char ssid[kSsidSize];
    int8_t rssi;
    uint8_t channel;
    uint8_t auth;  // wifi_auth_mode_t
    bool secured;
};

class Radio {
   public:
    Radio() = default;
    Radio(const Radio &) = delete;
    Radio &operator=(const Radio &) = delete;

    esp_err_t Init();
    bool Ready() const { return ready_; }

    // The Wi-Fi stack (and lwIP's netifs) exist: anything opening a socket
    // needs this, not just Ready().
    bool StackUp() const { return stack_ready_; }

    // DHCP hostname; applied when the stack next comes up.
    void SetHostname(const char *hostname);

    // Join `ssid` (empty password: open network), replacing the current
    // attempt. `keep_ap`: keep our AP up (APSTA); otherwise it goes down.
    // `ip` (copied) null or disabled: DHCP, put back if a previous network
    // had switched it off.
    esp_err_t StartClient(const char *ssid, const char *password, bool keep_ap,
                          const StaticIp *ip = nullptr);

    // Become an access point (empty password: open; shorter than 8 chars is
    // refused). Ends any station attempt.
    esp_err_t StartAp(const char *ssid, const char *password, uint8_t channel);

    // Radio and Wi-Fi stack down, heap given back. Idempotent.
    esp_err_t Stop();

    Status Get() const;

    // Blocking (a second or two). Works with the radio off: brings up what it
    // needs and puts it back. An attempt in flight is abandoned (reported as
    // a failure). Refused while another scan runs.
    esp_err_t Scan(ScanResult *out, size_t capacity, size_t *found);
    bool Scanning() const { return scanning_; }

    static const char *Name(Mode mode);
    static const char *Name(Link link);
    static const char *Name(Failure failure);
    static const char *AuthName(uint8_t auth);
    static Failure Classify(uint8_t reason);

   private:
    static void EventTrampoline(void *arg, const char *base, int32_t id, void *data);
    void OnEvent(const char *base, int32_t id, void *data);

    void SetLink(Link link, Failure failure, uint8_t reason);
    esp_err_t EnsureStack();
    void ReleaseStack();
    esp_err_t EnsureStarted();
    // Ends the current station attempt; its disconnect event is ours.
    void DropStation();
    // On association: the static address, or the DHCP client back on.
    void ApplyAddressing();
    esp_err_t SetDns(uint32_t address, int type);

    bool ready_ = false;
    bool stack_ready_ = false;  // esp_wifi_init done
    bool started_ = false;      // esp_wifi_start done
    bool ap_up_ = false;
    bool want_connect_ = false;  // connect on STA_START
    volatile bool scanning_ = false;
    // Disconnect events to ignore (ours). Only the manager task tears an
    // association down; the event task only consumes.
    volatile uint8_t suppress_disconnect_ = 0;

    char hostname_[kHostnameSize] = "esp32-auth";
    StaticIp static_ip_ = {};  // a copy: the config can change meanwhile
    Status status_ = {};
    // Short sections only, never held across an esp_wifi_* call (the event
    // task takes it too).
    SemaphoreHandle_t lock_ = nullptr;
    StaticSemaphore_t lock_storage_ = {};

    void *sta_netif_ = nullptr;
    void *ap_netif_ = nullptr;
};

}  // namespace wifi
