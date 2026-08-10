#pragma once
/*
 * Host stand-in for the header CMake generates per build.
 *
 * The real one is a compile-time constant. Here it is a variable, so a test can
 * change it and call auth_store_init() again — which is exactly what flashing a
 * new firmware looks like to the store: same flash contents, different id.
 * That transition is the one behaviour the whole feature rests on, so it has to
 * be reachable from a test rather than only from a soldering iron.
 */
#include <stdint.h>
extern uint32_t test_build_id;
#define NETHID_BUILD_ID     test_build_id
#define NETHID_BUILD_ID_STR "hosttest"
