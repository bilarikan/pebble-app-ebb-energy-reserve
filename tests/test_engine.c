// Host-side tests for the score engine. Build and run with `make -C tests`.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include "../src/c/demo_data.h"
#include "../src/c/engine.h"
#include "../src/c/model.h"

static int s_failed;
#define CHECK(cond, ...)                                              \
  do {                                                                \
    if (!(cond)) {                                                    \
      s_failed++;                                                     \
      printf("  FAIL %s:%d: ", __FILE__, __LINE__);                   \
      printf(__VA_ARGS__);                                            \
      printf("\n");                                                   \
    }                                                                 \
  } while (0)

#define DAY 1440u
// Midnight UTC on some day; tz 0 keeps local time equal to UTC in the tests.
#define T0 (29800000u - 29800000u % DAY)

static HrvWin s_wins[2048];

static Engine *fresh(uint32_t start, uint32_t end) {
  static Engine e;
  memset(&e, 0, sizeof(e));
  e.max_hr = M_DEFAULT_MAX_HR;
  engine_reset(&e, start);
  engine_rebase(&e);
  const uint16_t n = demo_windows(start, end, 0, s_wins, 2048);
  engine_set_windows(&e, s_wins, n);
  return &e;
}

static void run(Engine *e, uint32_t to) {
  for (uint32_t t = e->st.last_min + 1; t <= to; t++) {
    EngMinute m;
    demo_minute(t, 0, &m);
    engine_minute(e, t, &m);
  }
}

static void test_ordinary_days(void) {
  printf("ordinary days\n");
  Engine *e = fresh(T0, T0 + 6 * DAY);
  int lo = 100, hi = 0;
  for (int d = 0; d < 5; d++) {
    for (int h = 0; h < 24; h++) {
      run(e, T0 + d * DAY + h * 60 + 59);
      const int sc = engine_score(e);
      if (d >= 1) { if (sc < lo) lo = sc; if (sc > hi) hi = sc; }
      if (d == 4) printf("  day 4 %02d:59  score %3d\n", h, sc);
    }
  }
  CHECK(e->night_count == 5, "expected 5 nights, got %d", e->night_count);
  CHECK(!engine_calibrating(e), "baseline should be established after 5 nights");
  CHECK(e->base_rmssd10 > 350 && e->base_rmssd10 < 490, "baseline rmssd10 %d", e->base_rmssd10);
  CHECK(e->base_rhr >= 50 && e->base_rhr <= 56, "resting hr %d", e->base_rhr);
  CHECK(hi >= 90, "a good night should recharge to 90+, peak was %d", hi);
  CHECK(lo >= 10 && lo <= 60, "a workout day should bottom out between 10 and 60, got %d", lo);
  CHECK(!(e->st.flags & (EF_STALE | EF_LOW_CONF)), "flags %d", e->st.flags);

  // Morning charge is the reserve at wake, and the ledger reconciles with the score.
  run(e, T0 + 5 * DAY + 15 * 60);
  const int32_t expect = (int32_t)e->st.r0_100 - engine_since_wake(e, CAT_ACT) -
                         engine_since_wake(e, CAT_STRESS) + engine_since_wake(e, CAT_REST) +
                         engine_since_wake(e, CAT_SLEEP);
  CHECK(expect == e->st.reserve100, "ledger %d vs reserve %d", (int)expect, e->st.reserve100);
  CHECK(engine_since_wake(e, CAT_ACT) > 0 && engine_since_wake(e, CAT_STRESS) > 0,
        "activity and stress should both have drained by 15:00");
  printf("  baseline %d.%d ms, resting HR %d, range %d..%d\n", e->base_rmssd10 / 10,
         e->base_rmssd10 % 10, e->base_rhr, lo, hi);
}

static void test_reason_line(void) {
  printf("reason line\n");
  Engine *e = fresh(T0, T0 + 3 * DAY);
  run(e, T0 + 2 * DAY + 17 * 60 + 30);      // just before the workout
  engine_mark_viewed(e);
  const int before = e->st.reserve100;
  run(e, T0 + 2 * DAY + 18 * 60 + 20);      // after it
  const int32_t act = engine_since_view(e, CAT_ACT);
  CHECK(act >= 1000, "a 50 minute hard workout should cost 10+ points, cost %d/100", (int)act);
  CHECK(engine_since_view(e, CAT_SLEEP) == 0, "no sleep during a workout");
  CHECK(before - e->st.reserve100 == act, "drop %d vs activity %d", before - e->st.reserve100,
        (int)act);
}

static void test_not_worn(void) {
  printf("not worn\n");
  Engine *e = fresh(T0, T0 + 4 * DAY);
  run(e, T0 + 2 * DAY + 13 * 60);
  const int before = e->st.reserve100;
  // Watch off the wrist for four hours: records exist but carry no heart rate or steps.
  for (uint32_t t = e->st.last_min + 1; t <= T0 + 2 * DAY + 17 * 60; t++) {
    EngMinute m = { .flags = MF_VALID };
    engine_minute(e, t, &m);
  }
  CHECK(e->st.flags & EF_STALE, "should be stale after four hours without heart rate");
  // The hold period runs out after M_NOT_WORN_MIN; before that the wearer is treated as resting.
  CHECK(abs(e->st.reserve100 - before) <= 150, "should freeze, moved %d/100",
        e->st.reserve100 - before);
  const int frozen = e->st.reserve100;
  for (uint32_t t = e->st.last_min + 1; t <= T0 + 2 * DAY + 19 * 60; t++) {
    engine_minute(e, t, NULL);               // no records at all
  }
  CHECK(e->st.reserve100 == frozen, "missing minutes must not change the score");
  CHECK(e->curve[((T0 + 2 * DAY + 18 * 60) / CURVE_SLOT_MIN) % CURVE_SLOTS] == CURVE_NONE,
        "curve should show a gap");

  run(e, T0 + 2 * DAY + 20 * 60);
  CHECK(!(e->st.flags & EF_STALE), "worn again: no longer stale");
  CHECK(e->st.flags & EF_LOW_CONF, "worn again: low confidence until the next full night");
  run(e, T0 + 3 * DAY + 8 * 60);
  CHECK(!(e->st.flags & EF_LOW_CONF), "a full worn night restores confidence");
}

static void test_calibration(void) {
  printf("calibration\n");
  Engine *e = fresh(T0, T0 + 4 * DAY);
  run(e, T0 + 12 * 60);                     // the run starts at midnight, mid-sleep
  CHECK(engine_calibrating(e), "one night is not a baseline");
  CHECK(e->night_count == 1, "nights %d", e->night_count);
  run(e, T0 + DAY + 12 * 60);
  CHECK(engine_calibrating(e) && e->night_count == 2, "two nights are not a baseline either");
  run(e, T0 + 2 * DAY + 12 * 60);
  CHECK(!engine_calibrating(e), "three nights make a baseline");
  engine_clear_baseline(e);
  CHECK(engine_calibrating(e) && e->night_count == 0, "reset baseline");
}

static void test_no_hrv(void) {
  printf("sampler off\n");
  Engine *e = fresh(T0, T0 + 5 * DAY);
  engine_set_windows(e, NULL, 0);
  run(e, T0 + 4 * DAY + 12 * 60);
  CHECK(engine_calibrating(e), "no HRV windows: the baseline never forms");
  CHECK(e->night_count == 5 && e->nights[4].rmssd10 == 0 && e->nights[4].rhr > 0,
        "nights are still recorded, with resting HR but no HRV (%d)", e->night_count);
  const int sc = engine_score(e);
  CHECK(sc > 20 && sc <= 100, "score still runs on heart rate, sleep and motion: %d", sc);
}

static void test_low_hrv_night(void) {
  printf("poor night\n");
  Engine *good = fresh(T0, T0 + 6 * DAY);
  run(good, T0 + 5 * DAY + 7 * 60);
  const int r0_good = good->st.r0_100;

  Engine *e = fresh(T0, T0 + 6 * DAY);
  // Halve overnight HRV on the last night only.
  for (uint16_t i = 0; i < e->win_count; i++) {
    if ((s_wins[i].t & WIN_ASLEEP) && (s_wins[i].t & WIN_T_MASK) > T0 + 4 * DAY + 20 * 60) {
      s_wins[i].rmssd10 /= 2;
    }
  }
  run(e, T0 + 5 * DAY + 7 * 60);
  CHECK(e->st.r0_100 + 1000 < r0_good, "half the usual HRV should cut the morning charge: %d vs %d",
        e->st.r0_100, r0_good);
  printf("  morning charge %d vs %d on a normal night\n", e->st.r0_100 / 100, r0_good / 100);
}

static void test_skip(void) {
  printf("long gap\n");
  Engine *e = fresh(T0, T0 + 2 * DAY);
  run(e, T0 + DAY);
  const int before = e->st.reserve100;
  engine_skip_to(e, T0 + 9 * DAY);
  CHECK(e->st.reserve100 == before && (e->st.flags & EF_STALE), "gap freezes and marks stale");
  for (int i = 0; i < CURVE_SLOTS; i++) {
    if (e->curve[i] != CURVE_NONE) { CHECK(false, "curve slot %d not cleared", i); break; }
  }
}

int main(void) {
  printf("EngineState is %zu bytes (limit 256 per persist key)\n", sizeof(EngineState));
  CHECK(sizeof(EngineState) <= 256, "EngineState too large");
  CHECK(sizeof(Night) == 8 && sizeof(HrvWin) == 6, "packed sizes");
  CHECK(sizeof(HrvWin) * RING_PER_KEY <= 256, "ring page too large");
  test_ordinary_days();
  test_reason_line();
  test_not_worn();
  test_calibration();
  test_no_hrv();
  test_low_hrv_night();
  test_skip();
  printf(s_failed ? "\n%d check(s) FAILED\n" : "\nall checks passed\n", s_failed);
  return s_failed ? 1 : 0;
}
