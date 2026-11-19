// The scoring core, built into the worker as well as the app. ER_DEMO builds score in the
// app from synthetic data, so the worker leaves it out.
#ifndef ER_DEMO
#define ER_WORKER 1
#include "../../src/c/score.c"
#endif
