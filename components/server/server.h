#pragma once

// The authentication server (server_logic.h, device-protocol.md in
// ESP32AuthenticatorServer): the device's ECDSA P-256 key, TOTP secret and
// client id in `<dir>/server.bin` (SPIFFS, plain, as seed.txt), the
// registration, the sign-in (an access token, in memory), the status report
// and the ping, the firmware releases and their install, over HTTPS (the
// certificate bundle), and the TOTP code the user signs in with. Requests run in a task of their own, one at a time.

#include <cstddef>
#include <cstdint>

#include "esp_err.h"
#include "firmware_logic.h"
#include "server_logic.h"
#include "sync_logic.h"

namespace server {

// Loads `<dir>/server.bin` (none: not registered). Once, at boot.
esp_err_t Init(const char *dir);

// Where a store stands with the server (Settings > Server > Sync).
struct StoreStatus {
    sync::Phase phase;
    uint32_t local_version;   // our file's, 0 unknown
    uint32_t server_version;  // the server's copy (a conflict, a PIN asked), 0 unknown
    bool other_store;         // that copy is another store of the account (never synced)
    char server_at[17];       // when it reached the server ("2026-10-27 19:07" UTC), or ""
    uint8_t choices;          // a conflict with several such stores: how many (Choices)
};

// One of the account's stores a conflict can take (Choices), newest sync first.
struct StoreChoice {
    uint32_t version;
    char at[17];                // when it reached the server (UTC), or ""
    char by[kClientIdSize + 1];  // the device that sent it, "" unknown
    bool this_device;           // sent from here (under an old store)
};

struct Status {
    bool has_key;
    bool registered;
    bool has_client_id;      // from the registration's answer: signing in can go
    int64_t registered_at;   // 0: never
    int64_t last_sync;       // the last status the server took; 0: never
    Link link;
    bool busy;               // a request under way
    int http_status;         // the last answer's, 0: none
    bool has_offset;         // the server's clock is known (a ping, a status)
    int64_t offset;          // the device's clock minus the server's, seconds
    int64_t token_left;      // seconds the access token still has; 0: not signed in
    char update[kVersionSize];  // the release the server offers; empty: none
    char said[48];           // the last request's outcome
    StoreStatus stores[sync::kStores];  // by sync::Store
    sync::Icon icon;         // the status bar's
    int64_t synced_at;       // the last sync that went through (Unix seconds), 0: none
    char sync_said[48];      // the last sync's outcome
    bool others_known;       // the account's stores under other seed phrases looked at
    sync::Others others;     // those (shown only: they never open here)
    bool firmware_known;     // the releases looked at (after each status, or by hand)
    bool firmware_newer;     // one is newer than the running firmware: the status bar's icon
    uint8_t firmware_count;  // this board's releases kept (Releases)
    int64_t firmware_at;     // the last look that went through (Unix seconds), 0: none
    char firmware_said[48];  // the last look's or install's outcome
};
Status Get();

// The code for the server now (6 digits, 30 s); false: not registered.
bool Code(int64_t unix_seconds, uint32_t *code);

// A new key and TOTP secret (the old ones gone: the device is not registered
// from here), saved, then POSTed as a JWT to https://<host>/register in the
// task; the answer's client id kept. ESP_ERR_INVALID_STATE: a request is
// under way; ESP_ERR_INVALID_ARG: no host or e-mail. The caller sees the
// outcome in Get().
esp_err_t StartRegister(const char *host, const char *email);

// GET https://<host>/api/device/ping in the task: 200 {"status":"ok"} is
// "connected" (Link::kOk), its "time" the server's clock (Status::offset).
// ESP_ERR_INVALID_STATE: busy.
esp_err_t StartCheck(const char *host);

// What the vault and FIDO2 give the sync (from the server's task, 10 KB).
// kBad: the copy does not check out (its tag, an old version): a conflict,
// so this device's copy can be kept over it.
enum class Taken : uint8_t { kOk, kLocked, kNeedPin, kWrongPin, kBad, kFailed };

// What the schedule asks the device (any of them from the scheduler's task,
// 4 KB stack; `report` also from StartStatus' caller; the stores' from the
// server's task too).
struct Hooks {
    bool (*ready)();                          // Wi-Fi online, the clock set
    uint8_t (*status_minutes)();              // config.json server.statusMinutes
    void (*host)(char *out, size_t size);     // config.json server.host
    void (*report)(Report *out);              // the state (no `mac`: added here); static texts
    uint8_t (*sync_minutes)();                // config.json server.syncMinutes
    // A store's file; nullptr: none here (no card, refused, not loaded).
    const char *(*store_path)(sync::Store store);
    // Its loaded version, 0 while not open. Quick: a change shows.
    uint32_t (*store_version)(sync::Store store);
    // The PIN is in (a copy can be checked and taken).
    bool (*open)();
    // Holds nothing (no record, no credential); asked only when open.
    bool (*empty)(sync::Store store);
    // Our copy written again with a version of at least `min_version`.
    esp_err_t (*rewrite)(sync::Store store, uint32_t min_version);
    // The server's copy at `from` checked and put in place (vault::Adopt;
    // `from` gone after); `force`: any version taken, kept as it is.
    // kFailed: why in `said`.
    Taken (*take)(sync::Store store, const char *from,
                  const uint8_t store_id[sync::kStoreIdSize], bool force, const char *pin,
                  char *said, size_t size);
};

// The status schedule (a task of its own): the first status once the device
// is registered and ready (at the boot, or when Wi-Fi first connects), then
// every status_minutes (0: never); a failed one again in a minute. Once.
void StartSchedule(const Hooks *hooks);

// POST https://<host>/api/device/status now (signed in first when needed).
// ESP_ERR_INVALID_STATE: busy or no schedule; ESP_ERR_NOT_FOUND: not
// registered with a client id.
esp_err_t StartStatus();

// The stores' sync now (signed in first), also while a store waits for a
// person (it looks again). `by_hand`: the other seed phrases' stores looked
// at too (else at most every 10 min). ESP_ERR_INVALID_STATE: busy or no
// schedule; ESP_ERR_NOT_FOUND: not registered with a client id.
esp_err_t StartSync(bool by_hand = true);

// A store waiting for a person (sync::ByHand): keep this device's copy (its
// version above the server's, uploaded), or take the server's (with `pin`
// when it has another PIN: this device takes that PIN; `pick`: which of
// StoreStatus::choices, 0 the newest). ESP_ERR_INVALID_STATE: busy, or the
// store does not wait; ESP_ERR_INVALID_ARG: no such pick.
enum class Choice : uint8_t { kKeepMine, kTakeServer };
esp_err_t StartResolve(sync::Store store, Choice choice, const char *pin, uint8_t pick = 0);

// A conflict's stores to pick from (StoreStatus::choices): up to `max` into
// `out`; their count.
size_t Choices(sync::Store store, StoreChoice *out, size_t max);

// This board's releases on the server, newest first (firmware_logic.h), as
// last looked at: up to `max` into `out`; their count. Looked at after each
// status that went through, and by StartFirmwareList.
size_t Releases(firmware::Release *out, size_t max);

// The releases looked at now (signed in first). ESP_ERR_INVALID_STATE: busy
// or no schedule; ESP_ERR_NOT_FOUND: not registered with a client id.
esp_err_t StartFirmwareList();

// Release `id` (of the last look) downloaded in pieces into the other OTA
// slot (ota_update.h: its check as it streams, the progress window), its
// SHA-256 checked, then the restart into it. ESP_ERR_NOT_FOUND: not in the
// list, or not registered; ESP_ERR_INVALID_STATE: busy, or an update cannot
// start now (ota::Refusal).
esp_err_t StartInstall(const char *id);

// The public key, base64url (kPublicSize bytes); false: no key.
bool PublicKey(char *out, size_t size);

// The client id; false: none.
bool ClientId(char *out, size_t size);

}  // namespace server
