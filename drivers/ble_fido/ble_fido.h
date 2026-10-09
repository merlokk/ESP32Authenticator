#pragma once

#include <cstddef>
#include <cstdint>

// FIDO over BLE transport (CTAP 2.1 "Bluetooth Smart / BLE" binding): the FIDO
// GATT service 0xFFFD, request reassembly, response fragmentation by MTU, and
// PING/MSG/CANCEL framing. CTAP itself is a pluggable message handler.
// Based on the service layout of martin-ger/ESP32Auth (see sources.md).

namespace ble_fido {

// Handles one CTAP message (CTAP2 command or U2F APDU) and writes the response.
// Returns the response length (<= `out_size`).
using MessageHandler = size_t (*)(const uint8_t *msg, size_t len, uint8_t *out,
                                  size_t out_size);

// Adds the FIDO service and the event listener to the ble driver. Call once,
// before ble::On().
void Register();

// Replaces the default handler (which answers "command not supported").
void SetMessageHandler(MessageHandler handler);

// True when the host has subscribed to fidoStatus notifications.
bool Ready();

// Counters since boot: requests handled, errors sent.
void Stats(uint32_t *requests, uint32_t *errors);

}  // namespace ble_fido
