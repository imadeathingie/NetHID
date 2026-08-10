// Host-side harness: fake clock + fake matrix, so the pipeline can be tested
// without a Pico.
#include <stdint.h>
#include <stdbool.h>
#include <string.h>
#include <stdio.h>
#include "tusb.h"
#include "pico/stdlib.h"
#include "pico/sync.h"
#include "hardware/gpio.h"
#include "hardware/flash.h"
#include "hardware/sync.h"
#include "pico/multicore.h"

uint32_t fake_now_ms = 0;
bool     fake_sw[2][2];        // [row][col]

static int  pin_dir[32];
static int  asserted_pin = -1;

absolute_time_t get_absolute_time(void) { return (absolute_time_t){ (uint64_t)fake_now_ms * 1000 }; }
uint32_t to_ms_since_boot(absolute_time_t t) { return (uint32_t)(t.v / 1000); }
absolute_time_t make_timeout_time_ms(uint32_t ms) { return (absolute_time_t){ ((uint64_t)fake_now_ms + ms) * 1000 }; }
bool time_reached(absolute_time_t t) { return (uint64_t)fake_now_ms * 1000 >= t.v; }
void busy_wait_us_32(uint32_t) {}
void sleep_ms(uint32_t ms) { fake_now_ms += ms; }

uint32_t spin_lock_blocking(spin_lock_t*) { return 0; }
void spin_unlock(spin_lock_t*, uint32_t) {}
spin_lock_t* spin_lock_instance(unsigned) { return nullptr; }
int spin_lock_claim_unused(bool) { return 0; }

void gpio_init(uint32_t) {}
void gpio_set_function(uint32_t, uint32_t) {}
void gpio_put(uint32_t, bool) {}
void gpio_pull_up(uint32_t) {}
void gpio_disable_pulls(uint32_t) {}
void gpio_set_dir(uint32_t pin, int dir) {
    pin_dir[pin] = dir;
    if (dir == GPIO_OUT) asserted_pin = (int)pin;
    else if (asserted_pin == (int)pin) asserted_pin = -1;
}
#ifndef HARNESS_NO_GPIO_GET
/* encoder.cpp reads individual pins. Idle high, matching the internal pull-ups
 * a real encoder sits behind. The encoder test drives its own pins and defines
 * HARNESS_NO_GPIO_GET to keep this one out of the way. */
bool gpio_get(uint32_t) { return true; }
#endif

// rows GP2,GP3 driven; cols GP4,GP5 read (COL2ROW)
uint32_t gpio_get_all(void) {
    uint32_t v = 0xFFFFFFFFu;                     // pull-ups: all high
    if (asserted_pin == 2 || asserted_pin == 3) {
        int r = asserted_pin - 2;
        for (int c = 0; c < 2; c++) if (fake_sw[r][c]) v &= ~(1u << (4 + c));
    }
    return v;
}

/* When HARNESS_REAL_HID is defined the test links the real src/hid.cpp and
 * supplies its own endpoint, so these stubs must step aside. */
#ifndef HARNESS_REAL_HID
bool tud_hid_n_report(uint8_t, uint8_t, void const*, uint16_t) { return true; }
bool tud_mounted(void) { return true; }
bool tud_hid_n_ready(uint8_t) { return true; }
void tud_task(void) {}
bool tud_remote_wakeup(void) { return true; }
#endif

// ── Fake flash + multicore lockout ───────────────────────────────────────────
uint8_t fake_flash[PICO_FLASH_SIZE_BYTES];
void flash_range_erase(uint32_t off, size_t n) { memset(fake_flash + off, 0xFF, n); }
void flash_range_program(uint32_t off, const uint8_t *src, size_t n) { memcpy(fake_flash + off, src, n); }
uint32_t save_and_disable_interrupts(void) { return 0; }
void restore_interrupts(uint32_t) {}
void multicore_lockout_victim_init(void) {}
void multicore_lockout_start_blocking(void) {}
void multicore_lockout_end_blocking(void) {}
void multicore_fifo_drain(void) {}
void multicore_fifo_clear_irq(void) {}

// ── Mouse report capture ─────────────────────────────────────────────────────
#ifndef HARNESS_REAL_HID
static hid_mouse_report_t mrep[512];
static int mrep_n;
bool hid_push_mouse_report(const hid_mouse_report_t *r) {
    if (mrep_n < (int)(sizeof(mrep)/sizeof(mrep[0]))) mrep[mrep_n++] = *r;
    return true;
}
void    mouse_reset(void)      { mrep_n = 0; }
int     mouse_count(void)      { return mrep_n; }
int8_t  mouse_x(int i)         { return mrep[i].x; }
int8_t  mouse_y(int i)         { return mrep[i].y; }
int8_t  mouse_wheel(int i)     { return mrep[i].wheel; }
uint8_t mouse_buttons(int i)   { return mrep[i].buttons; }
#endif

#ifndef HARNESS_REAL_HID
// ── String typer ─────────────────────────────────────────────────────────────
// Modelled as taking a few "ms" so the macro interpreter's MS_TEXT wait is
// actually exercised rather than passing trivially.
static uint32_t typer_until;
bool hid_push_type_string(const char *t, uint8_t len, uint8_t) {
    printf("      [typer] \"%.*s\"\n", len, t);
    typer_until = fake_now_ms + 3;
    return true;
}
bool hid_typer_busy(void) { return fake_now_ms < typer_until; }
#endif  /* HARNESS_REAL_HID */
