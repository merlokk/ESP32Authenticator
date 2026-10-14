#pragma once

#include <cstddef>
#include <cstdint>

// FIDO over BLE transport (CTAP 2.1 "Bluetooth Smart / BLE" binding): the FIDO
// GATT service 0xFFFD, request reassembly, response fragmentation by MTU, and
// PING/MSG/CANCEL framing. CTAP itself is a pluggable message handler.
// Based on the service layout of martin-ger/ESP32Auth (see sources.md).

namespace ble_fido {

// Handles one CTAP message (CTAP2 command or U2F APDU) and writes the response.
// Returns the response length (<= `out_size`). Runs in the driver's worker
// task, one message at a time (a MSG meanwhile gets ERR_BUSY), so it may block
// waiting for the user; it should check Cancelled() and call Keepalive().
using MessageHandler = size_t (*)(const uint8_t *msg, size_t len, uint8_t *out,
                                  size_t out_size);

// Adds the FIDO service and the event listener to the ble driver. Call once,
// before ble::On().
void Register();

// Replaces the default handler (which answers "command not supported").
void SetMessageHandler(MessageHandler handler);

// Sends a KEEPALIVE frame (1 = processing, 2 = user presence needed) while a
// message is being handled. Call from the handler.
void Keepalive(uint8_t status);

// True when the host sent CANCEL (or disconnected) during the current message.
bool Cancelled();

// True when the host has subscribed to fidoStatus notifications.
bool Ready();

// Counters since boot: requests handled, errors sent.
void Stats(uint32_t *requests, uint32_t *errors);

}  // namespace ble_fido
