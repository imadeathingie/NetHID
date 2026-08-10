#pragma once
#include "kb/kb.h"

#ifndef ONESHOT_TIMEOUT_MS
#define ONESHOT_TIMEOUT_MS 3000   /* 0 = never expire */
#endif
#ifndef ONESHOT_TAP_TOGGLE
#define ONESHOT_TAP_TOGGLE 2      /* taps to lock a one-shot on; 0 = disable */
#endif

void kb_oneshot_cancel(void);
uint8_t kb_oneshot_mods(void);
