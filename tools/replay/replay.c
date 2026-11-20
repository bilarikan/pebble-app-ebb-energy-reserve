// Replays a watch's per-minute record through the real engine, on a computer.
//
//   tools/replay/run.sh DATADIR "DD HH:MM" RESERVE [M_NAME=value ...]
//
// DATADIR comes from parse_log.py. The replay starts at the given local time with the given
// reserve (0 to 100) and the baseline nights recorded before that day, and prints the score
// every hour next to what the watch itself showed at that time (from its 24-hour curve).
// MARKS="DD HH:MM,DD HH:MM" prints those minutes instead of every hour.
//
// It feeds engine.c directly, so it shows what the model does with a record. It does not
// run score.c: no late sleep labels, no anchors.
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include "engine.h"
#include "model.h"

#define MAX_WINS  1024
#define MAX_CURVE 512
#define MAX_MARKS 64

static HrvWin s_wins[MAX_WINS];
static unsigned s_curve_t[MAX_CURVE];
static int s_curve_v[MAX_CURVE], s_curve_n;
static int s_tz;

static FILE *open_in(const char *dir, const char *name) {
  char path[1024];
  snprintf(path, sizeof(path), "%s/%s", dir, name);
  FILE *f = fopen(path, "r");
  if (!f) { fprintf(stderr, "cannot read %s\n", path); exit(1); }
  return f;
}

// Local day of month, hour and minute of an epoch minute.
static void local(unsigned t, int *day, int *hh, int *mm) {
  const time_t secs = ((time_t)t + s_tz) * 60;
  struct tm tm;
  gmtime_r(&secs, &tm);
  *day = tm.tm_mday; *hh = tm.tm_hour; *mm = tm.tm_min;
}

static int watch_score(unsigned t) {
  for (int i = 0; i < s_curve_n; i++) {
    if (s_curve_t[i] / 15 == t / 15) return s_curve_v[i];
  }
  return -1;
}

int main(int argc, char **argv) {
  if (argc < 4) {
    fprintf(stderr, "usage: replay DATADIR \"DD HH:MM\" RESERVE\n");
    return 1;
  }
  const char *dir = argv[1];
  int sd, sh, sm;
  if (sscanf(argv[2], "%d %d:%d", &sd, &sh, &sm) != 3) { fprintf(stderr, "start is \"DD HH:MM\"\n"); return 1; }
  const double start_reserve = atof(argv[3]);

  FILE *f = open_in(dir, "meta.txt");
  if (fscanf(f, "%d", &s_tz) != 1) return 1;
  fclose(f);

  int nw = 0;
  unsigned t, a, r;
  f = open_in(dir, "wins.txt");
  while (nw < MAX_WINS && fscanf(f, "%u %u %u", &t, &a, &r) == 3) {
    s_wins[nw].t = (t & WIN_T_MASK) | (a ? WIN_ASLEEP : 0);
    s_wins[nw].rmssd10 = (uint16_t)r;
    nw++;
  }
  fclose(f);

  f = open_in(dir, "curve.txt");
  while (s_curve_n < MAX_CURVE && fscanf(f, "%u %d", &s_curve_t[s_curve_n], &s_curve_v[s_curve_n]) == 2) s_curve_n++;
  fclose(f);

  int marks[MAX_MARKS][3], nm = 0;
  if (getenv("MARKS")) {
    char *copy = strdup(getenv("MARKS"));
    for (char *tok = strtok(copy, ","); tok && nm < MAX_MARKS; tok = strtok(NULL, ",")) {
      if (sscanf(tok, "%d %d:%d", &marks[nm][0], &marks[nm][1], &marks[nm][2]) == 3) nm++;
    }
  }

  static Engine e;
  memset(&e, 0, sizeof(e));
  e.max_hr = getenv("MAX_HR") ? (uint8_t)atoi(getenv("MAX_HR")) : M_DEFAULT_MAX_HR;
  e.tz_min = (int16_t)s_tz;

  f = open_in(dir, "minutes.txt");
  unsigned steps, hr, vmc, flags;
  bool started = false;
  unsigned prev[CAT_COUNT] = { 0 };
  printf("   time     replay  watch  flags | since last line: activity stress rest sleep off-wrist\n");
  while (fscanf(f, "%u %u %u %u %u", &t, &steps, &hr, &vmc, &flags) == 5) {
    int d, h, m;
    local(t, &d, &h, &m);
    if (!started) {
      if (d != sd || h != sh || m != sm) continue;
      engine_reset(&e, t - 1);
      // The baseline as it stood: nights recorded before the starting day.
      const unsigned start_day = (unsigned)(((long)t + s_tz) / 1440);
      FILE *nf = open_in(dir, "nights.txt");
      unsigned day, rm, sl, rh, wn;
      while (e.night_count < NIGHTS_MAX && fscanf(nf, "%u %u %u %u %u", &day, &rm, &sl, &rh, &wn) == 5) {
        if (day > start_day) continue;
        e.nights[e.night_count++] = (Night){ .day = (uint16_t)day, .rmssd10 = (uint16_t)rm,
                                             .sleep_min = (uint16_t)sl, .rhr = (uint8_t)rh, .wins = (uint8_t)wn };
      }
      fclose(nf);
      engine_rebase(&e);
      engine_set_windows(&e, s_wins, (uint16_t)nw);
      e.st.flags = 0;
      e.st.reserve100 = (uint16_t)(start_reserve * 100);
      e.st.r0_100 = e.st.reserve100;
      e.st.wake_min = t - 1;   // treated as just woken, so time off the wrist is estimated
      e.st.tot[CAT_ACT] = 1;   // not a first run
      prev[CAT_ACT] = 1;
      started = true;
      printf("%02d %02d:%02d  %6.1f         start, %d nights, baseline RMSSD %d.%d ms, resting HR %d\n", d, h, m,
             start_reserve, e.night_count, e.base_rmssd10 / 10, e.base_rmssd10 % 10, e.base_rhr);
    }
    EngMinute min = { .vmc = (uint16_t)vmc, .steps = (uint8_t)steps, .hr = (uint8_t)hr, .flags = (uint8_t)flags };
    engine_minute(&e, t, &min);

    bool show = nm == 0 && m == 0;
    for (int i = 0; i < nm; i++) show |= marks[i][0] == d && marks[i][1] == h && marks[i][2] == m;
    if (!show) continue;
    const int w = watch_score(t);
    char wbuf[8] = "  -";
    if (w >= 0) snprintf(wbuf, sizeof(wbuf), "%3d", w);
    printf("%02d %02d:%02d  %6.1f   %s   %3d   | %6.1f %6.1f %6.1f %6.1f %6.1f\n", d, h, m, e.st.reserve100 / 100.0, wbuf,
           e.st.flags, (e.st.tot[0] - prev[0]) / 100.0, (e.st.tot[1] - prev[1]) / 100.0,
           (e.st.tot[2] - prev[2]) / 100.0, (e.st.tot[3] - prev[3]) / 100.0, (e.st.tot[4] - prev[4]) / 100.0);
    memcpy(prev, e.st.tot, sizeof(prev));
  }
  fclose(f);
  if (!started) { fprintf(stderr, "start time %s is not in the record\n", argv[2]); return 1; }
  printf("end       %6.1f  nights %d, baseline RMSSD %d.%d ms, resting HR %d\n", e.st.reserve100 / 100.0,
         e.night_count, e.base_rmssd10 / 10, e.base_rmssd10 % 10, e.base_rhr);
  return 0;
}
