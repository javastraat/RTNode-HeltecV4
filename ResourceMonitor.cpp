// ResourceMonitor.cpp — see ResourceMonitor.h.
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#include "ResourceMonitor.h"

#ifdef RESOURCE_MONITOR

#include <WiFi.h>
#include <esp_heap_caps.h>
#include <soc/soc_memory_types.h>
#include <Utilities/OS.h>

namespace res {

uint32_t cause_us[CAUSE_COUNT] = {0};
uint32_t cause_ops[CAUSE_COUNT] = {0};

namespace {

const uint32_t STALL_REPORT_US = 250000;
const uint32_t FLASH_REPORT_US = 20000;
const uint32_t REPORT_INTERVAL_MS = 60000;

// Loop-iteration latency buckets, upper bounds in microseconds.
const uint32_t BUCKET_US[] = {1000, 5000, 20000, 100000, 1000000};
const size_t BUCKETS = sizeof(BUCKET_US) / sizeof(BUCKET_US[0]) + 1;  // last is "over 1 s"

struct Window {
    uint32_t loops;
    uint32_t loop_max_us;
    uint32_t bucket[BUCKETS];
    uint32_t cause_us[CAUSE_COUNT];
    uint32_t cause_ops[CAUSE_COUNT];
    uint32_t lora_drops;
    uint16_t lora_queue_high_water;
};

Window window = {};
uint32_t loop_start_us = 0;
uint32_t loop_start_cause_us[CAUSE_COUNT] = {0};
uint32_t loop_start_cause_ops[CAUSE_COUNT] = {0};
uint32_t last_report_ms = 0;
uint32_t window_start_cause_us[CAUSE_COUNT] = {0};
uint32_t window_start_cause_ops[CAUSE_COUNT] = {0};

const char* tlsf_location() {
    void* pool = (void*)RNS::Utilities::OS::_tlsf;
    if (pool == nullptr) return "off(malloc)";
    return esp_ptr_external_ram(pool) ? "psram" : "internal";
}

uint32_t p99_bound_ms(const Window& w) {
    if (w.loops == 0) return 0;
    uint32_t allowed_over = w.loops / 100;  // iterations allowed above the p99 bound
    uint32_t above = 0;
    for (size_t i = BUCKETS; i-- > 0;) {
        above += w.bucket[i];
        if (above > allowed_over) {
            return i < BUCKETS - 1 ? BUCKET_US[i] / 1000 : 0xFFFFFFFF;
        }
    }
    return BUCKET_US[0] / 1000;
}

void report() {
    uint32_t delta_us[CAUSE_COUNT];
    uint32_t delta_ops[CAUSE_COUNT];
    for (size_t c = 0; c < CAUSE_COUNT; c++) {
        delta_us[c] = cause_us[c] - window_start_cause_us[c];
        delta_ops[c] = cause_ops[c] - window_start_cause_ops[c];
        window_start_cause_us[c] = cause_us[c];
        window_start_cause_ops[c] = cause_ops[c];
    }
    uint32_t p99 = p99_bound_ms(window);
    char p99_text[12];
    if (p99 == 0xFFFFFFFF) snprintf(p99_text, sizeof(p99_text), ">1000");
    else snprintf(p99_text, sizeof(p99_text), "<=%lu", (unsigned long)p99);

    Serial.printf(
        "[RES] t=%lu heap=%u/%u/%u psram=%u/%u tlsf=%s loops=%lu max=%lums p99%sms "
        "over20=%lu over100=%lu over1000=%lu lora_tx=%lums/%lu flash=%lums/%lu tcp=%lums/%lu "
        "q_hw=%u q_drop=%lu wifi=%d/%d\r\n",
        (unsigned long)millis(),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_minimum_free_size(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_INTERNAL),
        (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
        (unsigned)heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM),
        tlsf_location(),
        (unsigned long)window.loops,
        (unsigned long)(window.loop_max_us / 1000),
        p99_text,
        (unsigned long)(window.bucket[3] + window.bucket[4] + window.bucket[5]),
        (unsigned long)(window.bucket[4] + window.bucket[5]),
        (unsigned long)window.bucket[5],
        (unsigned long)(delta_us[LORA_TX] / 1000), (unsigned long)delta_ops[LORA_TX],
        (unsigned long)(delta_us[FLASH] / 1000), (unsigned long)delta_ops[FLASH],
        (unsigned long)(delta_us[TCP] / 1000), (unsigned long)delta_ops[TCP],
        (unsigned)window.lora_queue_high_water,
        (unsigned long)window.lora_drops,
        (int)WiFi.status(), (int)WiFi.RSSI());
    window = Window{};
}

}  // namespace

FlashOp::~FlashOp() {
    uint32_t elapsed = micros() - _start;
    cause_us[FLASH] += elapsed;
    cause_ops[FLASH]++;
    if (elapsed >= FLASH_REPORT_US) {
        Serial.printf("[FLASH] t=%lu %lums %s %s\r\n", (unsigned long)millis(),
                      (unsigned long)(elapsed / 1000), _op, _path ? _path : "");
    }
}

void loop_begin() {
    loop_start_us = micros();
    for (size_t c = 0; c < CAUSE_COUNT; c++) {
        loop_start_cause_us[c] = cause_us[c];
        loop_start_cause_ops[c] = cause_ops[c];
    }
}

void loop_end() {
    uint32_t elapsed = micros() - loop_start_us;
    window.loops++;
    if (elapsed > window.loop_max_us) window.loop_max_us = elapsed;
    size_t b = 0;
    while (b < BUCKETS - 1 && elapsed > BUCKET_US[b]) b++;
    window.bucket[b]++;

    if (elapsed >= STALL_REPORT_US) {
        uint32_t attributed = 0;
        for (size_t c = 0; c < CAUSE_COUNT; c++) attributed += cause_us[c] - loop_start_cause_us[c];
        uint32_t other = elapsed > attributed ? elapsed - attributed : 0;
        Serial.printf("[STALL] t=%lu %lums lora_tx=%lums/%lu flash=%lums/%lu tcp=%lums/%lu other=%lums\r\n",
                      (unsigned long)millis(), (unsigned long)(elapsed / 1000),
                      (unsigned long)((cause_us[LORA_TX] - loop_start_cause_us[LORA_TX]) / 1000),
                      (unsigned long)(cause_ops[LORA_TX] - loop_start_cause_ops[LORA_TX]),
                      (unsigned long)((cause_us[FLASH] - loop_start_cause_us[FLASH]) / 1000),
                      (unsigned long)(cause_ops[FLASH] - loop_start_cause_ops[FLASH]),
                      (unsigned long)((cause_us[TCP] - loop_start_cause_us[TCP]) / 1000),
                      (unsigned long)(cause_ops[TCP] - loop_start_cause_ops[TCP]),
                      (unsigned long)(other / 1000));
    }

    uint32_t now = millis();
    if (now - last_report_ms >= REPORT_INTERVAL_MS) {
        last_report_ms = now;
        report();
    }
}

void boot_report() {
    Serial.printf("[RES] boot psram_size=%u psram_free=%u tlsf=%s internal_free=%u\r\n",
                  (unsigned)ESP.getPsramSize(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_SPIRAM),
                  tlsf_location(),
                  (unsigned)heap_caps_get_free_size(MALLOC_CAP_INTERNAL));
    last_report_ms = millis();
}

void note_lora_queued(uint16_t queue_height) {
    if (queue_height > window.lora_queue_high_water) window.lora_queue_high_water = queue_height;
}

void note_lora_drop() {
    window.lora_drops++;
}

}  // namespace res

#endif  // RESOURCE_MONITOR
