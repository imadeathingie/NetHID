#pragma once
#include <stdbool.h>
void multicore_lockout_victim_init(void);
void multicore_lockout_start_blocking(void);
void multicore_lockout_end_blocking(void);
#define __no_inline_not_in_flash_func(f) f
void multicore_fifo_drain(void);
void multicore_fifo_clear_irq(void);
