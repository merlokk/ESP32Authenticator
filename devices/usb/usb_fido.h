#pragma once

// FIDO over USB HID (CTAP 2.1 "USB HID" binding, CTAPHID) on the serial+fido
// port (usb.h): 64-byte reports, channels, INIT / PING / WINK / MSG (U2F) /
// CBOR (CTAP2) / CANCEL, KEEPALIVE while a request runs. The framing and the
// channels are LionKey's (lib/lionkey/core/ctaphid.c); CTAP itself is a
// pluggable message handler, as for ble_fido.

#include <cstddef>
#include <cstdint>

namespace usb::fido {

// Handles one CTAP message (CTAP2 command or U2F APDU) and writes the response.
// Returns the response length (<= `out_size`). Runs in the transport's worker
// task, one message at a time (other channels meanwhile get CHANNEL_BUSY), so
// it may wait for the user; it should check Cancelled() and call Keepalive().
using MessageHandler = size_t (*)(const uint8_t *msg, size_t len, uint8_t *out,
                                  size_t out_size);

// Replaces the default handler (which answers "command not supported").
void SetMessageHandler(MessageHandler handler);

// Sends a KEEPALIVE (1 = processing, 2 = user presence needed) on the
// request's channel. Call from the handler.
void Keepalive(uint8_t status);

// True when the host sent CANCEL, or the port went away, during the request.
bool Cancelled();

// True in the transport's worker task (the CTAP core's hooks tell the USB
// request from a BLE one by it).
bool InWorker();

// Counters since boot: requests handled, errors sent, reports dropped (the
// queue full).
void Stats(uint32_t *requests, uint32_t *errors, uint32_t *dropped);

// --- usb.cpp

// The key's interface is up (allocates the buffers and tasks the first
// time) / going down (a running request is cancelled; nothing is sent).
void Start();
void Stop();

// One OUT report (TinyUSB's task): queued for the transport's task.
void OnReport(const uint8_t *report, size_t len);

// An IN report went out (TinyUSB's task).
void OnSent();

}  // namespace usb::fido
