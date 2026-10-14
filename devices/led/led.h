#pragma once

// One WS2812, driven from a UART (encoding: led_frames.h), and a task that keeps
// it showing what it was last told. Knows nothing about what the colours mean;
// the pin comes from the board. Ported from approver-esp32-yubikey.
//
// The task sleeps as long as the animator says the frame is good for (a solid
// colour: 1 s, a breath: 50 ms).

#include <cstdint>

#include "driver/gpio.h"
#include "driver/uart.h"
#include "esp_err.h"
#include "led_frames.h"

namespace led {

inline constexpr uint32_t kTaskStackBytes = 2560;
inline constexpr int kTaskPriority = 2;
// Longest sleep with nothing changing: a glitched pixel is corrected within it.
inline constexpr uint32_t kRefreshMs = 1000;

struct Status {
    bool ready = false;
    Rgb colour{};
    Effect effect = Effect::kSolid;
    uint8_t percent = 0;
    uint8_t idle_percent = 0;
    bool overriding = false;

    uint32_t writes = 0;
    uint32_t write_failures = 0;
    uint32_t stack_low_water = 0;
};

// UART driver on `port`, TX on `pin` inverted, and the task. Dark until Set().
esp_err_t Init(uart_port_t port, gpio_num_t pin);
bool Ready();

// Brightness ceilings in percent: for Set() and for SetIdle().
void SetBrightness(uint8_t percent, uint8_t idle_percent);

void Set(Rgb colour, Effect effect);
// The same at the idle ceiling, for resting states.
void SetIdle(Rgb colour, Effect effect);

// Shown for `duration_ms`, then back to what was underneath.
void SetFor(Rgb colour, Effect effect, uint32_t duration_ms);
void EndFor();

void Off();

Status Get();

}  // namespace led
