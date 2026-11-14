// Synthetic health data for the emulator and the host tests. Compiled only with ER_DEMO.
#pragma once
#ifdef ER_DEMO
#include "engine.h"

// One deterministic "ordinary day": sleep 23:10-06:40, desk work with a tense hour,
// a lunch walk, an evening workout and a quiet evening.
void demo_minute(uint32_t t_min, int16_t tz_min, EngMinute *out);

// HRV windows the worker would have produced in [from_min, to_min). Returns the count.
uint16_t demo_windows(uint32_t from_min, uint32_t to_min, int16_t tz_min, HrvWin *out,
                      uint16_t cap);
#endif
