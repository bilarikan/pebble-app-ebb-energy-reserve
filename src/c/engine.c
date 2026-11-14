#include "engine.h"
#include <string.h>
#include "model.h"

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

// ---- Small helpers -------------------------------------------------------------------------
static void sort_u16(uint16_t *a, int n) {
  for (int i = 1; i < n; i++) {
    uint16_t v = a[i];
    int j = i - 1;
    while (j >= 0 && a[j] > v) { a[j + 1] = a[j]; j--; }
    a[j + 1] = v;
  }
}

static uint16_t median_u16(uint16_t *a, int n) {
  if (n <= 0) return 0;
  sort_u16(a, n);
  return (n & 1) ? a[n / 2] : (uint16_t)((a[n / 2 - 1] + a[n / 2]) / 2);
}

static uint32_t win_time(const HrvWin *w) { return w->t & WIN_T_MASK; }
static bool win_asleep(const HrvWin *w) { return (w->t & WIN_ASLEEP) != 0; }

// ---- Baseline ------------------------------------------------------------------------------
void engine_rebase(Engine *e) {
  uint16_t tmp[NIGHTS_MAX];
  int n = 0;
  for (int i = 0; i < e->night_count; i++) {
    if (e->nights[i].rmssd10) tmp[n++] = e->nights[i].rmssd10;
  }
  e->base_nights = (uint8_t)n;
  e->base_rmssd10 = median_u16(tmp, n);

  n = 0;
  for (int i = 0; i < e->night_count; i++) {
    if (e->nights[i].rhr) tmp[n++] = e->nights[i].rhr;
  }
  e->base_rhr = (uint8_t)median_u16(tmp, n);
}

bool engine_calibrating(const Engine *e) {
  return e->base_nights < M_MIN_BASE_NIGHTS;
}

void engine_clear_baseline(Engine *e) {
  e->night_count = 0;
  memset(e->nights, 0, sizeof(e->nights));
  e->nights_dirty = true;
  engine_rebase(e);
}

const Night *engine_last_night(const Engine *e) {
  return e->night_count ? &e->nights[e->night_count - 1] : NULL;
}

static void night_upsert(Engine *e, const Night *n, bool same_sleep) {
  // A sleep that resumes after a short wake ends more than once: replace its record.
  if (same_sleep && e->night_count) {
    e->nights[e->night_count - 1] = *n;
  } else {
    if (e->night_count == NIGHTS_MAX) {
      memmove(&e->nights[0], &e->nights[1], sizeof(Night) * (NIGHTS_MAX - 1));
      e->night_count--;
    }
    e->nights[e->night_count++] = *n;
  }
  e->nights_dirty = true;
  engine_rebase(e);
}

// ---- HRV window lookup ---------------------------------------------------------------------
void engine_set_windows(Engine *e, const HrvWin *wins, uint16_t count) {
  e->wins = wins;
  e->win_count = count;
  e->wc_from = 1;
  e->wc_until = 0;   // empty cache
}

// Latest window at or before minute t. The replay moves forward in time, so the answer is
// cached until the next window starts.
static bool win_latest(Engine *e, uint32_t t) {
  if (!(t >= e->wc_from && t < e->wc_until)) {
    uint32_t best = 0, next = UINT32_MAX;
    e->wc_rmssd10 = 0;
    for (uint16_t i = 0; i < e->win_count; i++) {
      const HrvWin *w = &e->wins[i];
      const uint32_t wt = win_time(w);
      if (!w->rmssd10 || !wt) continue;
      if (wt <= t) {
        if (wt >= best) {
          best = wt;
          e->wc_rmssd10 = w->rmssd10;
          e->wc_asleep = win_asleep(w);
        }
      } else if (wt < next) {
        next = wt;
      }
    }
    e->wc_t = best;
    e->wc_from = t;
    e->wc_until = next;
  }
  return e->wc_t != 0;
}

// A fresh window of the right kind (asleep or awake) as a percentage of its reference:
// the overnight baseline while asleep, a fixed share of it while awake. -1 if there is none.
static int hrv_quotient_pct(Engine *e, uint32_t t, bool asleep) {
  if (engine_calibrating(e) || !e->base_rmssd10) return -1;
  if (!win_latest(e, t)) return -1;
  if (t - e->wc_t > M_HRV_FRESH_MIN || e->wc_asleep != asleep) return -1;
  uint32_t ref = e->base_rmssd10;
  if (!asleep) ref = ref * M_DAY_REF_PCT / 100;
  if (!ref) return -1;
  return (int)((uint32_t)e->wc_rmssd10 * 100u / ref);
}

// Recharge multiplier from HRV: below baseline slows the refill, above speeds it a little.
static int hrv_mult_pct(Engine *e, uint32_t t, bool asleep) {
  const int q = hrv_quotient_pct(e, t, asleep);
  if (q < 0) return 100;
  if (q <= 50) return M_HRV_MULT_MIN_PCT;
  if (q <= 100) return M_HRV_MULT_MIN_PCT + (q - 50) * (100 - M_HRV_MULT_MIN_PCT) / 50;
  if (q >= 130) return M_HRV_MULT_MAX_PCT;
  return 100 + (q - 100) * (M_HRV_MULT_MAX_PCT - 100) / 30;
}

// ---- Curve ---------------------------------------------------------------------------------
static void curve_put(Engine *e, uint32_t t, bool worn) {
  e->curve[(t / CURVE_SLOT_MIN) % CURVE_SLOTS] = worn ? (uint8_t)engine_score(e) : CURVE_NONE;
}

// ---- Main sleep ended ----------------------------------------------------------------------
static void on_wake(Engine *e, uint32_t t) {
  EngineState *s = &e->st;
  static uint16_t vals[128];
  int n = 0;
  for (uint16_t i = 0; i < e->win_count && n < 128; i++) {
    const HrvWin *w = &e->wins[i];
    const uint32_t wt = win_time(w);
    if (w->rmssd10 && win_asleep(w) && wt >= s->sleep_start_min && wt <= t) {
      vals[n++] = w->rmssd10;
    }
  }

  // Resting heart rate: a low percentile of the readings taken while asleep.
  uint32_t total = 0;
  for (int i = 0; i < RHR_BINS; i++) total += s->rhr_hist[i];
  uint8_t rhr = 0;
  if (total >= M_RHR_MIN_SAMPLES) {
    const uint32_t target = total * M_RHR_PERCENTILE / 100;
    uint32_t seen = 0;
    for (int i = 0; i < RHR_BINS; i++) {
      seen += s->rhr_hist[i];
      if (seen > target) { rhr = (uint8_t)(RHR_BIN0 + i); break; }
    }
  }

  Night night = {
    .day = (uint16_t)(((int32_t)t + e->tz_min) / 1440),
    .rmssd10 = (n >= M_MIN_NIGHT_WINDOWS) ? median_u16(vals, n) : 0,
    .sleep_min = s->sleep_min,
    .rhr = rhr,
    .wins = (uint8_t)clampi(n, 0, 255),
  };
  night_upsert(e, &night, e->night_count && s->night_sleep_start == s->sleep_start_min);
  s->night_sleep_start = s->sleep_start_min;

  s->wake_min = t;
  s->r0_100 = s->reserve100;
  memcpy(s->wake_tot, s->tot, sizeof(s->tot));
  // A full worn night re-anchors the score; without HRV it stays low confidence.
  if (night.rmssd10 || engine_calibrating(e)) {
    s->flags &= ~EF_LOW_CONF;
  } else {
    s->flags |= EF_LOW_CONF;
  }
}

// ---- One minute ----------------------------------------------------------------------------
void engine_minute(Engine *e, uint32_t t, const EngMinute *m) {
  EngineState *s = &e->st;
  const bool valid = m && (m->flags & MF_VALID);

  if (valid && m->hr >= 30 && m->hr <= 220) {
    s->last_hr = m->hr;
    s->last_hr_min = t;
  }
  const uint32_t hr_age = s->last_hr_min ? t - s->last_hr_min : UINT32_MAX;
  const bool hr_fresh = hr_age <= M_HR_HOLD_MIN;

  // Worn means a recent heart rate or steps. Sleep alone does not count: the watch's sleep
  // detection is motion-based and a watch on a table can look asleep.
  const bool worn = valid && (m->steps > 0 || hr_age <= M_NOT_WORN_MIN);
  if (!worn) {
    s->flags |= EF_STALE;
    if (s->flags & EF_ASLEEP) {
      s->flags &= ~EF_ASLEEP;
      if (s->sleep_min >= M_MAIN_SLEEP_MIN) on_wake(e, t);
    }
    curve_put(e, t, false);
    s->last_min = t;
    return;
  }
  if (s->flags & EF_STALE) {
    s->flags = (s->flags & ~EF_STALE) | EF_LOW_CONF;
  }

  const bool asleep = (m->flags & MF_ASLEEP) != 0;
  const bool was_asleep = (s->flags & EF_ASLEEP) != 0;
  if (asleep) {
    if (!was_asleep) {
      const bool resume = s->sleep_min && s->last_asleep_min &&
                          t - s->last_asleep_min <= M_SLEEP_MERGE_MIN;
      if (!resume) {
        s->sleep_start_min = t;
        s->sleep_min = 0;
        memset(s->rhr_hist, 0, sizeof(s->rhr_hist));
      }
      s->flags |= EF_ASLEEP;
    }
    if (s->sleep_min < UINT16_MAX) s->sleep_min++;
    s->last_asleep_min = t;
    if (m->hr >= RHR_BIN0 && m->hr < RHR_BIN0 + RHR_BINS && s->rhr_hist[m->hr - RHR_BIN0] < 255) {
      s->rhr_hist[m->hr - RHR_BIN0]++;
    }
  } else if (was_asleep) {
    s->flags &= ~EF_ASLEEP;
    if (s->sleep_min >= M_MAIN_SLEEP_MIN) on_wake(e, t);
  }

  // ---- Rates: exactly one of the four terms applies each minute ----
  const int rhr = e->base_rhr ? e->base_rhr : M_DEFAULT_RHR;
  const int max_hr = e->max_hr > rhr + 20 ? e->max_hr : rhr + 20;
  int delta = 0;
  int cat = -1;

  if (asleep) {
    delta = M_G_SLEEP * hrv_mult_pct(e, t, true) / 100;
    if (m->flags & MF_RESTFUL) delta = delta * M_RESTFUL_PCT / 100;
    cat = CAT_SLEEP;
  } else {
    int inten = hr_fresh ? clampi((s->last_hr - rhr) * 100 / (max_hr - rhr), 0, 100) : -1;
    const bool moving = m->steps >= M_STEPS_MOVING ||
                        (inten >= M_INTEN_ACTIVE_PCT && m->vmc >= M_VMC_MOVING);
    if (moving) {
      if (inten < 0) inten = clampi(m->steps * 100 / 160, 0, 100) * 60 / 100;   // from cadence
      delta = -(M_D_ACT_BASE + M_D_ACT_GAIN * inten * inten / 10000);
      cat = CAT_ACT;
      s->last_move_min = t;
    } else if (s->last_move_min && t - s->last_move_min <= M_COOLDOWN_MIN) {
      // Heart rate coming down after exertion: neither stress nor rest.
    } else if (hr_fresh) {
      const int s_hr =
          clampi((s->last_hr - rhr - M_STRESS_HR_MARGIN) * 100 / M_STRESS_HR_SPAN, 0, 100);
      const int q = hrv_quotient_pct(e, t, false);
      const int s_hrv =
          q < 0 ? 0 : clampi((M_STRESS_HRV_Q_PCT - q) * 100 / M_STRESS_HRV_SPAN, 0, 100);
      const int level = s_hr > s_hrv ? s_hr : s_hrv;
      if (level > 0) {
        delta = -(M_D_STRESS_BASE + M_D_STRESS_GAIN * level / 100);
        cat = CAT_STRESS;
      } else {
        delta = M_G_REST * hrv_mult_pct(e, t, false) / 100;
        cat = CAT_REST;
      }
    }
  }

  // Ledgers record what was actually applied after clamping, so they always add up.
  const int before = s->reserve100;
  const int after = clampi(before + delta, 0, 10000);
  s->reserve100 = (uint16_t)after;
  if (cat >= 0) s->tot[cat] += (uint32_t)(after > before ? after - before : before - after);

  curve_put(e, t, true);
  s->last_min = t;
}

void engine_skip_to(Engine *e, uint32_t t) {
  EngineState *s = &e->st;
  if (t <= s->last_min) return;
  s->flags = (s->flags & ~EF_ASLEEP) | EF_STALE;
  if (t - s->last_min >= CURVE_SLOTS * CURVE_SLOT_MIN) {
    memset(e->curve, CURVE_NONE, sizeof(e->curve));
  } else {
    for (uint32_t m = s->last_min + 1; m <= t; m += CURVE_SLOT_MIN) {
      e->curve[(m / CURVE_SLOT_MIN) % CURVE_SLOTS] = CURVE_NONE;
    }
    e->curve[(t / CURVE_SLOT_MIN) % CURVE_SLOTS] = CURVE_NONE;
  }
  s->sleep_min = 0;
  s->last_min = t;
}

void engine_reset(Engine *e, uint32_t start_min) {
  memset(&e->st, 0, sizeof(e->st));
  e->st.version = ENGINE_VERSION;
  e->st.flags = EF_LOW_CONF;
  e->st.reserve100 = M_START_RESERVE100;
  e->st.r0_100 = M_START_RESERVE100;
  e->st.last_min = start_min;
  e->st.view_min = start_min;
  e->st.view_reserve100 = M_START_RESERVE100;
  memset(e->curve, CURVE_NONE, sizeof(e->curve));
}

void engine_mark_viewed(Engine *e) {
  memcpy(e->st.view_tot, e->st.tot, sizeof(e->st.tot));
  e->st.view_min = e->st.last_min;
  e->st.view_reserve100 = e->st.reserve100;
}

int engine_score(const Engine *e) {
  return clampi((e->st.reserve100 + 50) / 100, 0, 100);
}

int engine_band(int score) {
  if (score >= M_BAND_HIGH) return 3;
  if (score >= M_BAND_MEDIUM) return 2;
  if (score >= M_BAND_LOW) return 1;
  return 0;
}

int32_t engine_since_view(const Engine *e, int cat) {
  return (int32_t)(e->st.tot[cat] - e->st.view_tot[cat]);
}

int32_t engine_since_wake(const Engine *e, int cat) {
  return (int32_t)(e->st.tot[cat] - e->st.wake_tot[cat]);
}
