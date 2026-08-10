/*
 * kb/features.h — the feature chain.
 *
 * Every behaviour beyond "matrix position → HID usage" lives in its own file
 * under src/kb/features/ and is toggled independently. A disabled feature is
 * not compiled, not linked, and not present in the dispatch table — it costs
 * nothing, not even a branch.
 *
 * ORDER MATTERS and is deliberately explicit below rather than being derived
 * from a linker section. Each feature's process hook returns:
 *
 *   true  — carry on down the chain
 *   false — swallow the event; nothing further sees it
 *
 * To add a feature:
 *   1. src/kb/features/<name>.cpp implementing kb_<name>_{init,process,task}
 *   2. a KB_FEATURE_<NAME> guard block below, placed at the right point
 *   3. one line in cmake/keyboard.cmake mapping the option to the source
 */
#pragma once

#include "kb/kb.h"

/* Defaults — a keyboard's rules.cmake or the command line overrides these. */
#ifndef KB_FEATURE_COMBO
#define KB_FEATURE_COMBO     0
#endif
#ifndef KB_FEATURE_CAPS_WORD
#define KB_FEATURE_CAPS_WORD 0
#endif
#ifndef KB_FEATURE_ONESHOT
#define KB_FEATURE_ONESHOT   0
#endif
#ifndef KB_FEATURE_TAPPING
#define KB_FEATURE_TAPPING   0
#endif
#ifndef KB_FEATURE_LAYERS
#define KB_FEATURE_LAYERS    0
#endif
#ifndef KB_FEATURE_MOUSEKEYS
#define KB_FEATURE_MOUSEKEYS 0
#endif
#ifndef KB_FEATURE_CONSUMER
#define KB_FEATURE_CONSUMER  0
#endif
#ifndef KB_FEATURE_AUTOCLICK
#define KB_FEATURE_AUTOCLICK 0
#endif
#ifndef KB_FEATURE_MACROS
#define KB_FEATURE_MACROS    0
#endif

/* Per-feature list entries, empty when the feature is off. */
#if KB_FEATURE_COMBO
#  define _KBF_COMBO      KB_FEATURE(combo)
#else
#  define _KBF_COMBO
#endif
#if KB_FEATURE_CAPS_WORD
#  define _KBF_CAPS_WORD  KB_FEATURE(caps_word)
#else
#  define _KBF_CAPS_WORD
#endif
#if KB_FEATURE_ONESHOT
#  define _KBF_ONESHOT    KB_FEATURE(oneshot)
#else
#  define _KBF_ONESHOT
#endif
#if KB_FEATURE_TAPPING
#  define _KBF_TAPPING    KB_FEATURE(tapping)
#else
#  define _KBF_TAPPING
#endif
#if KB_FEATURE_LAYERS
#  define _KBF_LAYERS     KB_FEATURE(layers)
#else
#  define _KBF_LAYERS
#endif
#if KB_FEATURE_MOUSEKEYS
#  define _KBF_MOUSEKEYS  KB_FEATURE(mousekeys)
#else
#  define _KBF_MOUSEKEYS
#endif
#if KB_FEATURE_CONSUMER
#  define _KBF_CONSUMER   KB_FEATURE(consumer)
#else
#  define _KBF_CONSUMER
#endif
#if KB_FEATURE_AUTOCLICK
#  define _KBF_AUTOCLICK  KB_FEATURE(autoclick)
#else
#  define _KBF_AUTOCLICK
#endif
#if KB_FEATURE_MACROS
#  define _KBF_MACROS     KB_FEATURE(macros)
#else
#  define _KBF_MACROS
#endif

/*
 * The chain, in processing order. Rationale for this ordering:
 *
 *   combo     first — it must see raw simultaneous presses before anything
 *                     rewrites or defers them
 *   caps_word next  — needs to observe every real keypress to decide whether
 *                     the word has ended, but must not see combo halves
 *   oneshot   next  — consumes the *next* key, so it has to run before the
 *                     tapping machine can defer that key
 *   tapping   next  — may hold an event back for up to TAPPING_TERM ms
 *   layers    next  — MO/TO/TG/DF/OSL act immediately once tapping resolved
 *   mousekeys next  — terminal actions, same tier as layers
 *   consumer  next  — likewise; its own HID report, nothing downstream cares
 *   autoclick next  — must sit AFTER the features whose keycodes it repeats,
 *                     since it re-enters the chain from the top to emit them
 *   macros    last  — user code, sees whatever survived
 */
#define KB_FEATURE_LIST \
    _KBF_COMBO          \
    _KBF_CAPS_WORD      \
    _KBF_ONESHOT        \
    _KBF_TAPPING        \
    _KBF_LAYERS         \
    _KBF_MOUSEKEYS      \
    _KBF_CONSUMER       \
    _KBF_AUTOCLICK      \
    _KBF_MACROS

/* Declare the hooks of every enabled feature. */
#define KB_FEATURE(name)                     \
    void kb_##name##_init(void);             \
    bool kb_##name##_process(keyrecord_t *); \
    void kb_##name##_task(void);
KB_FEATURE_LIST
#undef KB_FEATURE

typedef struct {
    void (*init)(void);
    bool (*process)(keyrecord_t *);
    void (*task)(void);
    const char *name;
} kb_feature_t;
