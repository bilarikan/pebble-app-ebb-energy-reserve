#ifdef ER_DEMO
#include "demo_data.h"

static uint32_t hash(uint32_t x) {
  x ^= x >> 16; x *= 0x7feb352du;
  x ^= x >> 15; x *= 0x846ca68bu;
  x ^= x >> 16;
  return x;
}

static int jitter(uint32_t seed, int span) {   // -span .. +span
  return (int)(hash(seed) % (uint32_t)(2 * span + 1)) - span;
}

#define HM(h, m) ((h) * 60 + (m))

static bool asleep_at(int lm) { return lm >= HM(23, 10) || lm < HM(6, 40); }

void demo_minute(uint32_t t, int16_t tz_min, EngMinute *out) {
  const int lm = (int)(((int32_t)t + tz_min) % 1440);
  int steps = 0, hr = 0, vmc = 30;
  uint8_t flags = MF_VALID;
  bool every_minute = false;   // the firmware samples heart rate more often when active

  if (asleep_at(lm)) {
    flags |= MF_ASLEEP;
    if ((lm >= HM(0, 0) && lm < HM(1, 0)) || (lm >= HM(2, 30) && lm < HM(3, 15)) ||
        (lm >= HM(4, 30) && lm < HM(5, 0))) {
      flags |= MF_RESTFUL;
    }
    hr = 54 + jitter(t, 3);
    vmc = 5;
  } else if (lm < HM(7, 30)) {                 // morning routine
    steps = (hash(t) % 3 == 0) ? 25 + (int)(hash(t + 7) % 20) : 4;
    hr = 80 + jitter(t, 6);
    vmc = 900;
  } else if (lm < HM(7, 55)) {                 // walk to work
    steps = 100 + jitter(t, 8);
    hr = 98 + jitter(t, 5);
    vmc = 3500;
    every_minute = true;
  } else if (lm < HM(12, 0)) {                 // desk, tense between 10 and 11
    const bool tense = lm >= HM(10, 0) && lm < HM(11, 0);
    hr = (tense ? 84 : 59) + jitter(t, 4);
    vmc = 120;
  } else if (lm < HM(12, 30)) {                // lunch walk
    steps = 92 + jitter(t, 8);
    hr = 96 + jitter(t, 5);
    vmc = 3200;
    every_minute = true;
  } else if (lm < HM(17, 30)) {                // desk
    hr = 59 + jitter(t, 4);
    vmc = 110;
  } else if (lm < HM(18, 10)) {                // workout
    steps = 150 + jitter(t, 10);
    hr = 148 + jitter(t, 6);
    vmc = 6000;
    every_minute = true;
  } else if (lm < HM(18, 40)) {                // cool down
    steps = 30 + jitter(t, 6);
    hr = 96 - (lm - HM(18, 10)) / 3 + jitter(t, 3);
    vmc = 1200;
    every_minute = true;
  } else {                                     // quiet evening
    hr = 55 + jitter(t, 3);
    vmc = 60;
  }

  out->steps = (uint8_t)steps;
  out->vmc = (uint16_t)vmc;
  out->hr = (every_minute || t % 10 == 0) ? (uint8_t)hr : 0;
  out->flags = flags;
}

uint16_t demo_windows(uint32_t from_min, uint32_t to_min, int16_t tz_min, HrvWin *out,
                      uint16_t cap) {
  uint16_t n = 0;
  for (uint32_t t = from_min - from_min % 10; t < to_min && n < cap; t += 10) {
    if (t < from_min) continue;
    const int lm = (int)(((int32_t)t + tz_min) % 1440);
    const uint32_t night = (uint32_t)(((int32_t)t + tz_min + 60) / 1440);   // varies per night
    int rmssd10;
    uint32_t flags = (uint32_t)7 << WIN_Q_SHIFT;
    if (asleep_at(lm)) {
      rmssd10 = 420 + jitter(night * 31u, 40) + jitter(t, 45);
      flags |= WIN_ASLEEP;
    } else {
      if (t % 30 != 0) continue;
      EngMinute m;
      demo_minute(t, tz_min, &m);
      if (m.steps) continue;                                   // the worker samples only when still
      const bool tense = lm >= HM(10, 0) && lm < HM(11, 0);
      rmssd10 = (tense ? 170 : 330) + jitter(t, 30);
    }
    out[n].t = (t & WIN_T_MASK) | flags;
    out[n].rmssd10 = (uint16_t)rmssd10;
    n++;
  }
  return n;
}
#endif
