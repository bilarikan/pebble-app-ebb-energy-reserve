// Scoring core: catches the engine up with the watch's own history and keeps its state in
// persistent storage. The same file is built into the background worker, which does the
// scoring, and into the app, which scores only when the worker is not running (and always
// in ER_DEMO builds, from synthetic data).
//
// Nothing is carried in memory between runs: a run starts from what is stored and ends by
// storing what it produced, so whichever process runs next carries on correctly.
#pragma once
#include "engine.h"
#include "shared.h"

uint32_t score_now_min(void);

// Reads the stored engine state, curve and nights; starts afresh when there is none.
// For display. A scoring run does this itself.
void score_load(Engine *e);

// One scoring run: prepare; if that succeeds, begin, then step until it returns false, then
// end (which stores the result). Each step handles a bounded stretch of history, so the
// caller can yield between. In the worker, allocate the Engine only after score_prepare():
// preparing needs about 2 KiB of free heap, which it gives back.
bool score_prepare(void);
void score_begin(Engine *e);
bool score_step(Engine *e);
void score_end(Engine *e);

// Stores the engine's records as they stand. For the process doing the scoring only.
void score_store(Engine *e, bool nights_too);

#ifndef ER_DEMO
// What the last run had to work with: sleep sessions the firmware reported (255 if the run
// was abandoned for lack of them) and the free heap, in bytes, just before they were read.
uint16_t score_run_sessions(void);
uint16_t score_run_heap(void);
#endif

#if defined(ER_DEBUG) && !defined(ER_WORKER) && !defined(ER_DEMO)
// The sleep sessions Pebble Health reports for [from, to), newest first.
uint8_t score_debug_spans(uint32_t from_min, uint32_t to_min);
bool score_debug_span(uint8_t i, uint32_t *a, uint32_t *b, bool *restful);
int score_debug_flags(uint32_t t_min);          // MF_ASLEEP | MF_RESTFUL for one minute
// Pretends one recent minute was scored with the other sleep label, so the next run rebuilds.
void score_debug_flip_label(const Engine *e);
#endif
