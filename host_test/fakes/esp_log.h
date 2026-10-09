#pragma once

// ESP log macros routed to fake::Log: silent unless fake::log_enabled, but a
// real varargs call so the format strings still compile.

namespace fake {
extern bool log_enabled;
void Log(const char *level, const char *tag, const char *format, ...);
}  // namespace fake

#define ESP_LOGE(tag, ...) ::fake::Log("E", tag, __VA_ARGS__)
#define ESP_LOGW(tag, ...) ::fake::Log("W", tag, __VA_ARGS__)
#define ESP_LOGI(tag, ...) ::fake::Log("I", tag, __VA_ARGS__)
#define ESP_LOGD(tag, ...) ::fake::Log("D", tag, __VA_ARGS__)
#define ESP_LOGV(tag, ...) ::fake::Log("V", tag, __VA_ARGS__)
