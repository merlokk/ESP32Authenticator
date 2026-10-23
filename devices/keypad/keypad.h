#pragma once

// The board's push buttons (X4 Pro: Left, Right, Power; dev board: BOOT as
// Left), owned by one task: polled every buttons::kPollIntervalMs, debounced,
// turned into events (key_events.h) in a queue for the UI, and the live state
// for the rest (FIDO2 user presence reads Left here). Nobody else reads the
// pins: one owner per button.

#include <cstdint>

#include "board.h"
#include "esp_err.h"
#include "key_events.h"

namespace keypad {

inline constexpr uint32_t kTaskStackBytes = 2560;
inline constexpr int kTaskPriority = 5;  // above the UI: a press is never missed
inline constexpr unsigned kQueueDepth = 16;

// After boards::Init. Configures the pins and starts the task.
esp_err_t Start();

bool Present(boards::ButtonId id);
// Debounced, as of the last poll.
bool Pressed(boards::ButtonId id);
// How long the current press has lasted; 0 when released.
uint32_t HeldMs(boards::ButtonId id);

// The next event (`key` is the ButtonId), waiting up to `timeout_ms`. A full
// queue drops new events (counted).
bool Next(buttons::KeyEvent *out, uint32_t timeout_ms);
// Empties the queue (a reader that starts now wants new events only).
void Flush();

// Console `keys`: per button its GPIO, raw and debounced level, held time.
void PrintInfo();

}  // namespace keypad
