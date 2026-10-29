#pragma once

// The USB port (usb_logic.h: the modes). The chip's one USB PHY is either the
// Serial/JTAG's (kJtag: as the ROM leaves it) or the OTG controller's, where
// TinyUSB shows a CDC-ACM serial port (kSerial) and beside it a FIDO2 / U2F
// key (kSerialFido, usb_fido.h). SetMode hands the PHY over at once, no
// restart: the host sees the old device leave and the new one arrive.
//
// stdin / stdout / stderr (the console and the log) are a device of ours,
// /dev/authusb, which follows the port in use: '\n' goes out as "\r\n", CR
// comes in as LF. Nobody reading (no host; or the host stopped taking it for
// 50 ms): output is dropped, never waited for. The serial port needs no DTR:
// its FIFO keeps the latest output until a terminal reads it.
//
// The Serial/JTAG gets the PHY back before a restart and a deep sleep, so
// the boot's own lines, and the ROM's download mode, are on it.

#include <cstddef>

#include "esp_err.h"
#include "freertos/FreeRTOS.h"
#include "usb_logic.h"

namespace usb {

// Puts stdin / stdout on the port and starts `mode` (the TinyUSB one failing:
// the Serial/JTAG, and an error; ESP_ERR_NOT_SUPPORTED: the OTG controller is
// off in eFuse, TinyUSB not tried). Once, early in app_main; not kReader.
esp_err_t Init(Mode mode);

// Switches the port; output in flight is flushed first (up to 50 ms). The
// TinyUSB one failing: back on the Serial/JTAG, and an error. kReader: the
// microSD card leaves FAT (sd_card::Lend) for the host's drive and comes back
// when another mode is set (no card: ESP_ERR_NOT_FOUND, nothing changed). A
// TinyUSB mode with the OTG controller off in eFuse: ESP_ERR_NOT_SUPPORTED,
// nothing changed.
esp_err_t SetMode(Mode mode);

// Called with true before the card goes to the host, false after it is back:
// whatever keeps files on the card in memory lets go of them (the vault).
void SetReaderHook(void (*hook)(bool on));

// The reader over: the mode before it back (not the reader: nothing). It
// also ends by itself when the host ejects the drive or is gone for 3 s.
esp_err_t EndReader();

// The host ejected the drive or went (TinyUSB's task): the reader ends.
void NoteEject();

// The port in use.
Mode GetMode();

// A host talks to the port: Serial/JTAG SOF frames, or TinyUSB configured
// and not suspended. A charger alone is no host.
bool HostConnected();

// Someone has the console open: the serial port's DTR; on the Serial/JTAG
// (no DTR) a host.
bool PortOpen();

// Console input past the command line (file transfers): up to `size` bytes,
// waiting up to `timeout` for the first; 0 when none came. CR comes as LF.
int Read(void *buf, size_t size, TickType_t timeout);

// `usb` (console): the mode, the host, the port, the FIDO2 key's counters.
void PrintInfo();

}  // namespace usb
