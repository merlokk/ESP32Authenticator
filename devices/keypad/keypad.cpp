#include "keypad.h"

#include <cinttypes>
#include <cstdio>

#include "board_devices.h"
#include "buttons.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/queue.h"
#include "freertos/task.h"

namespace keypad {

namespace {

constexpr const char *TAG = "keypad";
constexpr size_t kCount = boards::kButtonCount;

struct Key {
    buttons::Button pin{GPIO_NUM_NC};
    buttons::KeyTracker tracker;
};

Key keys[kCount];
// The trackers are written by the task and read by others (Pressed, HeldMs).
portMUX_TYPE lock = portMUX_INITIALIZER_UNLOCKED;

StaticQueue_t queue_storage;
uint8_t queue_buffer[kQueueDepth * sizeof(buttons::KeyEvent)];
QueueHandle_t queue = nullptr;
uint32_t dropped = 0;

StaticTask_t task_storage;
StackType_t task_stack[kTaskStackBytes / sizeof(StackType_t)];
bool started = false;

uint32_t NowMs() { return static_cast<uint32_t>(esp_timer_get_time() / 1000); }

void Task(void *) {
    TickType_t wake = xTaskGetTickCount();
    for (;;) {
        const uint32_t now = NowMs();
        for (Key &k : keys) {
            if (!k.pin.Present()) {
                continue;
            }
            const bool raw = k.pin.RawPressed();
            portENTER_CRITICAL(&lock);
            const buttons::KeyEvents events = k.tracker.Update(raw, now);
            portEXIT_CRITICAL(&lock);
            for (uint8_t i = 0; i < events.size; ++i) {
                if (xQueueSend(queue, &events.items[i], 0) != pdTRUE) {
                    ++dropped;
                }
            }
        }
        // At least one tick: kPollIntervalMs (10 ms) is one tick at 100 Hz.
        xTaskDelayUntil(&wake, pdMS_TO_TICKS(buttons::kPollIntervalMs) > 0
                                   ? pdMS_TO_TICKS(buttons::kPollIntervalMs)
                                   : 1);
    }
}

Key *Find(boards::ButtonId id) {
    const size_t i = static_cast<size_t>(id);
    return i < kCount && keys[i].pin.Present() ? &keys[i] : nullptr;
}

}  // namespace

esp_err_t Start() {
    if (started) {
        return ESP_OK;
    }
    queue = xQueueCreateStatic(kQueueDepth, sizeof(buttons::KeyEvent), queue_buffer,
                               &queue_storage);
    const uint32_t now = NowMs();
    for (size_t i = 0; i < kCount; ++i) {
        Key &k = keys[i];
        k.pin = boards::Button(static_cast<boards::ButtonId>(i));
        k.tracker = buttons::KeyTracker(static_cast<uint8_t>(i));
        if (!k.pin.Present()) {
            continue;
        }
        const esp_err_t err = k.pin.Init();
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "%s on GPIO%d: %s", boards::Name(static_cast<boards::ButtonId>(i)),
                     static_cast<int>(k.pin.Gpio()), esp_err_to_name(err));
            k.pin = buttons::Button(GPIO_NUM_NC);
            continue;
        }
        k.tracker.Reset(k.pin.RawPressed(), now);
    }
    // ESP-IDF counts the stack depth in bytes.
    if (xTaskCreateStatic(&Task, "keypad", kTaskStackBytes, nullptr, kTaskPriority, task_stack,
                          &task_storage) == nullptr) {
        return ESP_FAIL;
    }
    started = true;
    return ESP_OK;
}

bool Present(boards::ButtonId id) { return Find(id) != nullptr; }

bool Pressed(boards::ButtonId id) {
    const Key *k = Find(id);
    if (k == nullptr) {
        return false;
    }
    portENTER_CRITICAL(&lock);
    const bool pressed = k->tracker.Pressed();
    portEXIT_CRITICAL(&lock);
    return pressed;
}

uint32_t HeldMs(boards::ButtonId id) {
    const Key *k = Find(id);
    if (k == nullptr) {
        return 0;
    }
    const uint32_t now = NowMs();
    portENTER_CRITICAL(&lock);
    const uint32_t held = k->tracker.HeldMs(now);
    portEXIT_CRITICAL(&lock);
    return held;
}

bool Next(buttons::KeyEvent *out, uint32_t timeout_ms) {
    return queue != nullptr && xQueueReceive(queue, out, pdMS_TO_TICKS(timeout_ms)) == pdTRUE;
}

void Flush() {
    if (queue != nullptr) {
        xQueueReset(queue);
    }
}

void PrintInfo() {
    printf("task       %s, every %" PRIu32 " ms; long %" PRIu32 " ms, clicks end %" PRIu32
           " ms after the last release\n",
           started ? "running" : "not started", buttons::kPollIntervalMs, buttons::kLongMs,
           buttons::kMultiGapMs);
    for (size_t i = 0; i < kCount; ++i) {
        const auto id = static_cast<boards::ButtonId>(i);
        const Key *k = Find(id);
        if (k == nullptr) {
            printf("%-10s none\n", boards::Name(id));
            continue;
        }
        const uint32_t held = HeldMs(id);
        printf("%-10s GPIO%d, raw %s, %s", boards::Name(id), static_cast<int>(k->pin.Gpio()),
               k->pin.RawPressed() ? "down" : "up", Pressed(id) ? "pressed" : "released");
        if (held > 0) {
            printf(" for %" PRIu32 " ms", held);
        }
        printf("\n");
    }
    printf("queue      %u waiting, %" PRIu32 " dropped (full)\n",
           queue != nullptr ? static_cast<unsigned>(uxQueueMessagesWaiting(queue)) : 0U,
           dropped);
}

}  // namespace keypad
