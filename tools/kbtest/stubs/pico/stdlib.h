#pragma once
#include <stdint.h>
#include <stdbool.h>
typedef struct { uint64_t v; } absolute_time_t;
absolute_time_t get_absolute_time(void);
uint32_t to_ms_since_boot(absolute_time_t);
absolute_time_t make_timeout_time_ms(uint32_t);
bool time_reached(absolute_time_t);
void busy_wait_us_32(uint32_t);
void sleep_ms(uint32_t);
static inline void tight_loop_contents(void) {}
