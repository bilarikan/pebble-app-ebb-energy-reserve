// Scoring core shared by the worker and the app. See score.h.
#ifdef ER_WORKER
#include <pebble_worker.h>
#else
#include <pebble.h>
#endif
#include "score.h"
#include "model.h"
#ifdef ER_DEMO
#include "demo_data.h"
#endif

// The worker has 10 KiB for code and data together, and reading sleep sessions makes the
// firmware allocate a health cache of about 2 KiB from the caller's heap (2052 bytes on
// PebbleOS 4.44.4; with less free it returns no sessions and no error). So a run in the
// worker is in two parts that never hold memory at the same time: score_prepare() reads the
// sessions and has the cache released, and only then are the engine and buffers allocated.
#ifdef ER_WORKER
#define CHUNK_MIN 15
void worker_drop_health_cache(void);   // in worker.c
#else
#define CHUNK_MIN 60
#endif

uint32_t score_now_min(void) {
  return (uint32_t)(time(NULL) / 60);
}

static int16_t tz_offset_min(void) {
  const time_t now = time(NULL);
  const struct tm *lt = localtime(&now);
  int d = lt->tm_hour * 60 + lt->tm_min - (int)((now / 60) % 1440);
  if (d > 840) d -= 1440;
  if (d < -720) d += 1440;
  return (int16_t)d;
}

static bool s_fresh;   // the stored state was missing: this run starts from scratch

void score_load(Engine *e) {
  memset(e, 0, sizeof(*e));
  Settings set;
  const bool have_set = persist_read_data(PK_SETTINGS, &set, sizeof(set)) == (int)sizeof(set) &&
                        set.version == SETTINGS_VERSION;
  e->max_hr = have_set ? set.max_hr : M_DEFAULT_MAX_HR;
  e->tz_min = tz_offset_min();

  const int nights_bytes = persist_read_data(PK_NIGHTS, e->nights, sizeof(e->nights));
  e->night_count = nights_bytes > 0 ? (uint8_t)(nights_bytes / (int)sizeof(Night)) : 0;
  engine_rebase(e);

  s_fresh = !(persist_read_data(PK_ENGINE, &e->st, sizeof(e->st)) == (int)sizeof(e->st) &&
              e->st.version == ENGINE_VERSION &&
              persist_read_data(PK_CURVE, e->curve, sizeof(e->curve)) == (int)sizeof(e->curve));
  if (s_fresh) {
#ifdef ER_DEMO
    engine_reset(e, score_now_min() - 5 * 1440);   // five synthetic nights: a full baseline
#else
    engine_reset(e, score_now_min() - M_FIRST_LOOKBACK_MIN);
#endif
  }
}

void score_store(Engine *e, bool nights_too) {
  persist_write_data(PK_ENGINE, &e->st, sizeof(e->st));
  persist_write_data(PK_CURVE, e->curve, sizeof(e->curve));
  if (nights_too || e->nights_dirty) {
    if (e->night_count) {
      persist_write_data(PK_NIGHTS, e->nights, e->night_count * sizeof(Night));
    } else {
      persist_delete(PK_NIGHTS);
    }
  }
}

// ============================================================================================
#ifdef ER_DEMO
// Emulator: the same engine fed a synthetic day. App only.
static HrvWin *s_wins;

bool score_prepare(void) {
  return true;
}

void score_begin(Engine *e) {
  const uint32_t now = score_now_min();
  e->tz_min = tz_offset_min();
  if (e->st.last_min > now) e->st.last_min = now;
  const uint32_t from = e->st.last_min - 1440;
  const uint16_t cap = (uint16_t)((now - from) / 10 + 8);
  s_wins = malloc(sizeof(HrvWin) * cap);
  engine_set_windows(e, s_wins, s_wins ? demo_windows(from, now, e->tz_min, s_wins, cap) : 0);
}

bool score_step(Engine *e) {
  const uint32_t now = score_now_min();
  const uint32_t start = e->st.last_min + 1;
  if (start >= now) return false;
  const uint32_t end = MIN(start + CHUNK_MIN, now);
  for (uint32_t t = start; t < end; t++) {
    EngMinute m;
    demo_minute(t, e->tz_min, &m);
    engine_minute(e, t, &m);
  }
  return end < now;
}

void score_end(Engine *e) {
  engine_set_windows(e, NULL, 0);
  free(s_wins);
  s_wins = NULL;
  score_store(e, true);
}

// ============================================================================================
#else

// Pebble Health decides that a stretch was sleep up to an hour or more after it began, by
// which time those minutes may already have been scored as awake. So two earlier copies of
// the engine state are stored, with the sleep label each recent minute was scored with:
//   anchor: at least SETTLE_MIN old, so every label before it is final;
//   next:   the state at the end of a run, which becomes the anchor once it is that old.
// When a label changes on a minute already scored, the score is rebuilt from the anchor.
#define SETTLE_MIN   240   // how long after the fact a label may still change
#define SETTLE_STEP  15    // a long catch-up moves the anchor to a multiple of this
#define SETTLE_BITS  256   // minutes of labels remembered (more than SETTLE_MIN)
#define MAX_SPANS    16    // the firmware hands out at most this many sessions

typedef struct __attribute__((packed)) {
  uint8_t asleep[SETTLE_BITS / 8], restful[SETTLE_BITS / 8];   // indexed by minute % SETTLE_BITS
} SettleBits;

typedef struct __attribute__((packed)) {
  uint32_t a;                  // first minute
  uint16_t len : 15;           // minutes
  uint16_t restful : 1;        // a restful (deep) stretch, which is also sleep
} Span;

static SettleBits *s_bits;          // allocated for the run
static Span s_spans[MAX_SPANS];
static uint8_t s_span_n;
static uint8_t s_span_seen;         // sessions the firmware reported, kept or not
static uint32_t s_span_from;        // sessions that ended before this minute are not kept
static bool s_abort;                // this run could not read what it needs: store nothing
static uint16_t s_heap_free;        // free heap just before the sessions were read
static HealthMinuteData *s_buf;
static uint32_t s_anchor_min;       // last minute of the stored anchor, 0 = none
static uint32_t s_next_min;         // the same for `next`
static uint32_t s_anchor_target;    // minute at which this run should store a newer anchor
static uint32_t s_begin_now;        // "now" when the run began
static uint32_t s_pending_view;     // after a rebuild: when the owner last looked

// ---- HRV windows, one ring key at a time ----------------------------------------------------
static HrvWin *s_page;        // allocated for the run
static int8_t s_page_key;

static void ring_win_at(uint16_t i, HrvWin *out) {
  const int8_t key = (int8_t)(i / RING_PER_KEY);
  if (key != s_page_key) {
    memset(s_page, 0, sizeof(HrvWin) * RING_PER_KEY);
    persist_read_data(PK_RING0 + key, s_page, sizeof(HrvWin) * RING_PER_KEY);
    s_page_key = key;
  }
  *out = s_page[i % RING_PER_KEY];
}

// ---- Sleep sessions -------------------------------------------------------------------------
static bool activity_cb(HealthActivity activity, time_t start, time_t end, void *context) {
  const uint32_t a = (uint32_t)(start / 60), b = (uint32_t)((end + 59) / 60);
  s_span_seen++;
  if (b <= s_span_from || b <= a || s_span_n >= MAX_SPANS) return true;
  s_spans[s_span_n++] = (Span) {
    .a = a,
    .len = (uint16_t)MIN(b - a, 32767u),
    .restful = activity == HealthActivityRestfulSleep,
  };
  return true;
}

// Loads the sleep sessions that end after from_min. Returns false if the firmware gave
// none although it reports sleep today: it answers with nothing, and no error, when the
// process has too little free memory for its health cache. Scoring on would turn a night
// into waking hours.
static bool load_spans(uint32_t from_min, uint32_t to_min) {
  s_span_n = s_span_seen = 0;
  s_span_from = from_min;
  // Start a day early so a sleep already under way at from_min is found.
  health_service_activities_iterate(HealthActivitySleep | HealthActivityRestfulSleep,
                                    (time_t)(from_min - 1440) * 60, (time_t)to_min * 60,
                                    HealthIterationDirectionFuture, activity_cb, NULL);
  return s_span_seen || health_service_sum_today(HealthMetricSleepSeconds) == 0;
}

static int span_flags(uint32_t t) {
  int flags = 0;
  for (uint8_t i = 0; i < s_span_n; i++) {
    if (t >= s_spans[i].a && t - s_spans[i].a < s_spans[i].len) {
      flags |= s_spans[i].restful ? (MF_ASLEEP | MF_RESTFUL) : MF_ASLEEP;
    }
  }
  return flags;
}

static bool bit_get(const uint8_t *bits, uint32_t t) {
  return (bits[(t % SETTLE_BITS) >> 3] >> (t & 7)) & 1;
}
static void bit_set(uint8_t *bits, uint32_t t, bool on) {
  if (on) bits[(t % SETTLE_BITS) >> 3] |= (uint8_t)(1 << (t & 7));
  else    bits[(t % SETTLE_BITS) >> 3] &= (uint8_t)~(1 << (t & 7));
}

// The last minute of a stored copy of the engine state, 0 if it is missing or out of date.
static uint32_t stored_min(uint32_t key, uint32_t not_after) {
  struct __attribute__((packed)) { uint8_t version, flags; uint16_t reserve100; uint32_t last_min; } head;
  if (persist_get_size(key) != (int)sizeof(EngineState) ||
      persist_read_data(key, &head, sizeof(head)) != (int)sizeof(head) ||
      head.version != ENGINE_VERSION || head.last_min > not_after) {
    return 0;
  }
  return head.last_min;
}

// ---- One run --------------------------------------------------------------------------------
// Reads the sleep sessions this run will need. Call before allocating anything for the run.
// False means the run cannot go ahead (see load_spans); try again later.
bool score_prepare(void) {
  const uint32_t now = score_now_min();
  // As far back as a rebuild could reach: the anchor, or failing that where scoring stands.
  uint32_t from = stored_min(PK_ANCHOR, now);
  if (!from) from = stored_min(PK_ENGINE, now);
  if (!from || now - from > M_MAX_REPLAY_MIN) from = now - M_MAX_REPLAY_MIN;
  s_heap_free = (uint16_t)MIN(65535u, heap_bytes_free());
  s_abort = !load_spans(from, now + 1);
#ifdef ER_WORKER
  worker_drop_health_cache();   // the sessions are copied; the firmware's 2 KiB can go back
#endif
  return !s_abort;
}

void score_begin(Engine *e) {
  score_load(e);   // whoever scored last, carry on from what is stored
  const uint32_t now = score_now_min();
  if (!s_abort) {
    s_buf = malloc(sizeof(HealthMinuteData) * CHUNK_MIN);
    s_page = malloc(sizeof(HrvWin) * RING_PER_KEY);
    s_bits = malloc(sizeof(SettleBits));
    s_page_key = -1;
  }
  if (!s_buf || !s_page || !s_bits) {
    s_abort = true;
    return;
  }

  // The clock went backwards (time zone travel does not do this; a reset can).
  if (e->st.last_min > now) e->st.last_min = now;

  uint32_t looked;
  if (persist_read_data(PK_VIEW, &looked, sizeof(looked)) == (int)sizeof(looked) &&
      looked > e->st.view_min) {
    engine_mark_viewed(e);   // the owner has looked since: the reason line starts again here
  }

  // Older than the replay limit: nothing reliable to say about that stretch.
  if (now - e->st.last_min > M_MAX_REPLAY_MIN) {
    engine_skip_to(e, now - M_MAX_REPLAY_MIN);
  }

  s_anchor_min = s_fresh ? 0 : stored_min(PK_ANCHOR, e->st.last_min);
  s_next_min = s_fresh ? 0 : stored_min(PK_NEXT, e->st.last_min);
  if (s_next_min <= s_anchor_min) s_next_min = 0;
  if (s_fresh || persist_read_data(PK_SETTLE, s_bits, sizeof(*s_bits)) != (int)sizeof(*s_bits)) {
    memset(s_bits, 0, sizeof(*s_bits));
    s_anchor_min = s_next_min = 0;
  }


  // Have the sleep labels changed on minutes already scored since the anchor?
  s_pending_view = 0;
  if (s_anchor_min && e->st.last_min > s_anchor_min &&
      e->st.last_min - s_anchor_min <= M_MAX_REPLAY_MIN) {
    // Only the labels still remembered can be compared; anything older is final anyway.
    uint32_t t = s_anchor_min + 1;
    if (e->st.last_min - t >= SETTLE_BITS) t = e->st.last_min - (SETTLE_BITS - 1);
    for (; t <= e->st.last_min; t++) {
      const int f = span_flags(t);
      if (bit_get(s_bits->asleep, t) != ((f & MF_ASLEEP) != 0) ||
          bit_get(s_bits->restful, t) != ((f & MF_RESTFUL) != 0)) {
        break;
      }
    }
    if (t <= e->st.last_min) {
      // The owner's last look came after the anchor was taken; it is marked again when the
      // rebuild passes that minute, so the reason line still adds up.
      const uint32_t view_min = e->st.view_min;
      if (persist_read_data(PK_ANCHOR, &e->st, sizeof(e->st)) == (int)sizeof(e->st)) {
        if (view_min > s_anchor_min) s_pending_view = view_min;
      }
    }
  }

  // A long catch-up scores old minutes with labels that are already final: store a newer
  // anchor on the way past.
  s_begin_now = now;
  s_anchor_target = (now - SETTLE_MIN) / SETTLE_STEP * SETTLE_STEP;
  if (s_anchor_target <= s_anchor_min || s_anchor_target <= e->st.last_min) s_anchor_target = 0;

  engine_set_window_source(e, ring_win_at, RING_CAP);
}

// Scores one minute (`r` is NULL when the watch has no record for it) and remembers the
// sleep label it was scored with.
static void feed(Engine *e, uint32_t t, const HealthMinuteData *r) {
  const int f = span_flags(t);
  bit_set(s_bits->asleep, t, (f & MF_ASLEEP) != 0);
  bit_set(s_bits->restful, t, (f & MF_RESTFUL) != 0);
  if (r) {
    EngMinute m = {
      .vmc = r->vmc,
      .steps = r->steps,
      .hr = r->heart_rate_bpm,
      .flags = (uint8_t)((r->is_invalid ? 0 : MF_VALID) | f),
    };
    engine_minute(e, t, &m);
  } else {
    engine_minute(e, t, NULL);
  }
  if (s_pending_view && t >= s_pending_view) {
    engine_mark_viewed(e);
    s_pending_view = 0;
  }
  // A rebuild passing the minute `next` was taken at brings it up to date.
  if (t == s_next_min) persist_write_data(PK_NEXT, &e->st, sizeof(e->st));
  if (t == s_anchor_target) {
    persist_write_data(PK_ANCHOR, &e->st, sizeof(e->st));
    s_anchor_min = t;
    if (s_next_min <= s_anchor_min) s_next_min = 0;
  }
}

bool score_step(Engine *e) {
  const uint32_t now = score_now_min();
  const uint32_t start = e->st.last_min + 1;
  if (start >= now || s_abort) return false;
  const uint32_t end = MIN(start + CHUNK_MIN, now);   // exclusive

  time_t ts = (time_t)start * 60, te = (time_t)end * 60;
  const uint32_t n = health_service_get_minute_history(s_buf, CHUNK_MIN, &ts, &te);
  const uint32_t first = n ? (uint32_t)(ts / 60) : end;

  // Minutes before the first record returned are gaps for certain: the history is written in
  // order, so data after them means they will never arrive. (The call answers a range that
  // has no data with the next records it does have, which is how `first` can lie ahead.)
  // With nothing returned at all, the newest minutes may simply not have been written yet:
  // those count as gaps only once they are older than the lag.
  uint32_t t = start;
  for (; t < first && t < end; t++) {
    if (!n && t + M_RECENT_LAG_MIN >= now) return false;
    feed(e, t, NULL);
  }
  for (uint32_t i = 0; i < n; i++) {
    const uint32_t rt = first + i;
    if (rt < start) continue;
    if (rt >= end) break;
    feed(e, rt, &s_buf[i]);
    t = rt + 1;
  }
  for (; t < end; t++) {
    if (t + M_RECENT_LAG_MIN >= now) return false;
    feed(e, t, NULL);
  }
  return end < now;
}

void score_end(Engine *e) {
  engine_set_window_source(e, NULL, 0);
  free(s_buf);
  s_buf = NULL;
  if (s_abort) {   // nothing was scored and nothing is stored; the next run tries again
    free(s_page);
    free(s_bits);
    s_page = NULL;
    s_bits = NULL;
    return;
  }

  if (s_pending_view || s_fresh) {
    engine_mark_viewed(e);   // a first run has nothing earlier to compare with
    s_pending_view = 0;
    s_fresh = false;
  }

  // `next` is old enough for its labels to be final: it becomes the anchor. The ring page
  // doubles as the buffer for the copy.
  const uint32_t now = score_now_min();
  if (s_next_min && s_next_min + SETTLE_MIN <= now) {
    if (sizeof(HrvWin) * RING_PER_KEY >= sizeof(EngineState) &&
        persist_read_data(PK_NEXT, s_page, sizeof(EngineState)) == (int)sizeof(EngineState)) {
      persist_write_data(PK_ANCHOR, s_page, sizeof(EngineState));
      s_anchor_min = s_next_min;
    }
    s_next_min = 0;
  }
  free(s_page);
  s_page = NULL;
  if (!s_next_min && e->st.last_min > s_anchor_min) {
    persist_write_data(PK_NEXT, &e->st, sizeof(e->st));
  }
  persist_write_data(PK_SETTLE, s_bits, sizeof(*s_bits));
  free(s_bits);
  s_bits = NULL;
  score_store(e, false);
#if !defined(ER_WORKER) || defined(ER_WLOG)
  APP_LOG(APP_LOG_LEVEL_INFO, "scored to %d: %d flags %d r%d nights %d", (int)(e->st.last_min % 1440),
          engine_score(e), (int)e->st.flags, (int)e->st.reserve100, (int)e->night_count);
#endif
}

// What the last run had to work with: sessions the firmware reported (255 if the run was
// abandoned) and the free heap just before they were read. Sent to the app with WMSG_SCORED.
uint16_t score_run_sessions(void) {
  return s_abort ? 255 : s_span_seen;
}

uint16_t score_run_heap(void) {
  return s_heap_free;
}

// ---- Debug ----------------------------------------------------------------------------------
#if defined(ER_DEBUG) && !defined(ER_WORKER)
uint8_t score_debug_spans(uint32_t from_min, uint32_t to_min) {
  load_spans(from_min, to_min);
  return s_span_n;
}

bool score_debug_span(uint8_t i, uint32_t *a, uint32_t *b, bool *restful) {
  if (i >= s_span_n) return false;
  *a = s_spans[i].a;
  *b = s_spans[i].a + s_spans[i].len;
  *restful = s_spans[i].restful != 0;
  return true;
}

int score_debug_flags(uint32_t t_min) {
  return span_flags(t_min);
}

void score_debug_flip_label(const Engine *e) {
  const uint32_t t = e->st.last_min - 30;
  SettleBits bits;
  if (persist_read_data(PK_SETTLE, &bits, sizeof(bits)) != (int)sizeof(bits)) return;
  bit_set(bits.asleep, t, !bit_get(bits.asleep, t));
  persist_write_data(PK_SETTLE, &bits, sizeof(bits));
  APP_LOG(APP_LOG_LEVEL_INFO, "D flip label: anchor %d next %d now r%d tot %d %d %d %d %d",
          (int)(stored_min(PK_ANCHOR, e->st.last_min) % 1440), (int)(stored_min(PK_NEXT, e->st.last_min) % 1440),
          (int)e->st.reserve100, (int)e->st.tot[0], (int)e->st.tot[1], (int)e->st.tot[2],
          (int)e->st.tot[3], (int)e->st.tot[4]);
}
#endif

#endif   // !ER_DEMO
