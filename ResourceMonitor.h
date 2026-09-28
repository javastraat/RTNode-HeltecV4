// ResourceMonitor.h — bench instrumentation: where loop() time goes, and
// what the node has left. See PERFORMANCE_STRATEGY.md, "Instrumentation".
//
// Everything here runs on the loop task, so plain counters are enough.
// Output (firewall builds on ESP32; elsewhere this compiles to nothing):
//   [RES] boot ...    once, from setup(): PSRAM size and where the TLSF pool lives
//   [RES] ...         every 60 s: heap, loop latency, blocked time by cause
//   [STALL] ...       any loop() iteration of 250 ms or more, with its causes
//   [FLASH] ...       any single flash operation of 20 ms or more
//
// This program is free software: you can redistribute it and/or modify
// it under the terms of the GNU General Public License as published by
// the Free Software Foundation, either version 3 of the License, or
// (at your option) any later version.

#ifndef RESOURCE_MONITOR_H
#define RESOURCE_MONITOR_H

#include <Arduino.h>

#if defined(ESP32) && defined(FIREWALL_MODE)
#define RESOURCE_MONITOR 1
#endif

namespace res {

// Things that can hold loop() up while they finish. RX is Reticulum
// processing a received packet (any interface), JOBS its periodic work,
// BLE the Bluetooth interface's loop, SCREEN a display update (DISPLAY is an Arduino macro).
enum Cause : uint8_t { LORA_TX = 0, FLASH, TCP, RX, JOBS, BLE, SCREEN, CAUSE_COUNT };

#ifdef RESOURCE_MONITOR

extern uint32_t cause_us[CAUSE_COUNT];   // cumulative since boot
extern uint32_t cause_ops[CAUSE_COUNT];

// Adds its own lifetime to a cause. A timer started inside another pauses
// it, so each microsecond counts once, to the innermost cause (a flash write
// inside JOBS is FLASH, a packet received inside BLE is RX).
class Timed;
extern Timed* timed_current;

class Timed {
public:
    explicit Timed(Cause cause) : _cause(cause), _start(micros()), _outer(timed_current) {
        if (_outer) cause_us[_outer->_cause] += _start - _outer->_start;
        timed_current = this;
    }
    ~Timed() {
        uint32_t now = micros();
        cause_us[_cause] += now - _start;
        cause_ops[_cause]++;
        timed_current = _outer;
        if (_outer) _outer->_start = now;
    }
    Timed(const Timed&) = delete;
    Timed& operator=(const Timed&) = delete;
private:
    Cause _cause;
    uint32_t _start;
    Timed* _outer;
};

// Timed flash operation; logs itself when it takes 20 ms or more.
class FlashOp {
public:
    FlashOp(const char* op, const char* path) : _op(op), _path(path), _start(micros()), _timed(FLASH) {}
    ~FlashOp();
private:
    const char* _op;
    const char* _path;
    uint32_t _start;
    Timed _timed;
};

void loop_begin();
void loop_end();          // also emits [STALL] and the periodic [RES] line
void boot_report();
void note_lora_queued(uint16_t queue_height);
void note_lora_drop();
void note_lora_split_drop();   // half of a split LoRa packet discarded

#else

class Timed {
public:
    explicit Timed(Cause) {}
};

class FlashOp {
public:
    FlashOp(const char*, const char*) {}
};

inline void loop_begin() {}
inline void loop_end() {}
inline void boot_report() {}
inline void note_lora_queued(uint16_t) {}
inline void note_lora_drop() {}
inline void note_lora_split_drop() {}

#endif

}  // namespace res

#endif
