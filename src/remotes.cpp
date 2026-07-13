// ============================================================
//  remotes.cpp — IR blaster + 433 MHz OOK transmit, driven by PIO.
//
//  PIO generates the bit timing in hardware, so WiFi/USB activity on the CPU
//  cannot jitter the waveform. We feed timing words into each SM's TX FIFO; the
//  SM clocks them out at the exact rate set by its clock divider.
//
//  NOTE (hardware verification pending): the PIO timing here follows the
//  standard pico-examples patterns but has NOT been confirmed on a scope/logic
//  analyzer. Before trusting it, capture the output of the IR pin and confirm
//  ~38 kHz carrier during marks, and confirm the 433 pin reproduces the
//  captured pulse widths. See README "Verifying the remote timing".
// ============================================================
#include "remotes.h"
#include "config.h"
#include "hardware/pio.h"
#include "hardware/clocks.h"
#include "hardware/gpio.h"
#include "hardware/irq.h"

// Optional: power the IR receiver from a GPIO held HIGH so a 3-pin receiver can
// plug straight into [IR_RX_PIN | GND | IR_RX_PWR_PIN]. Older config.h files
// don't define this, and a bare `#if IR_RX_PWR_PIN >= 0` would silently treat an
// undefined macro as 0 and then fail to compile on the identifier. Default it to
// -1 (= use the 3V3 rail) when config.h doesn't set it.
#ifndef IR_RX_PWR_PIN
#define IR_RX_PWR_PIN (-1)
#endif
#include "pico/stdlib.h"
#include <stdio.h>

#include "rf_ook.pio.h"
#include "ir_tx.pio.h"

// ── Pin defaults (override in config.h) ───────────────────────────────────────
#ifndef IR_TX_PIN
#define IR_TX_PIN   16
#endif
#ifndef RF_TX_PIN
#define RF_TX_PIN   17
#endif
#ifndef IR_CARRIER_HZ
#define IR_CARRIER_HZ 38000
#endif

// We put IR on pio0 and RF on pio1 so the two never contend for FIFOs.
static PIO  ir_pio = pio0;
static uint ir_sm  = 0;
static uint ir_offset = 0;
static int  ir_carrier_loaded = 0;   // remembers the divider's carrier freq

static PIO  rf_pio = pio1;
static uint rf_sm  = 0;
static uint rf_offset = 0;

static bool g_inited = false;

// ── Receive capture (edge-timed, gap-framed) ─────────────────────────────────
#define RX_MAX_EDGES   256                       // matches the TX array limit
#define RF_SYNC_MIN_US 2500                       // real OOK packets have a long sync gap
static const uint32_t RX_GAP_US[2]    = { 12000, 8000 };  // idle gap => frame complete (IR, RF)
static const int      RX_MIN_EDGES[2] = { 8, 16 };        // reject runts / noise
static const uint8_t  RX_PIN[2]       = { IR_RX_PIN, RF_RX_PIN };

typedef struct {
    volatile uint32_t last_us;
    volatile uint16_t buf[RX_MAX_EDGES];
    volatile int      n;
    volatile bool     armed;
    volatile bool     have_first;
    volatile bool     ready;
    uint16_t          result[RX_MAX_EDGES];
    int               result_n;
    uint32_t          arm_deadline;
} rx_chan_t;

static rx_chan_t g_rx[2];

static inline void rx_irq_set(int k, bool en) {
    gpio_set_irq_enabled(RX_PIN[k], GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, en);
}

// Shared GPIO edge ISR (core 0). Records the duration of each completed level
// as a microsecond delta; the first edge only starts the clock.
static void rx_gpio_isr(uint gpio, uint32_t events) {
    (void)events;
    uint32_t now = time_us_32();
    for (int k = 0; k < 2; k++) {
        if (gpio != RX_PIN[k] || !g_rx[k].armed) continue;
        rx_chan_t *c = &g_rx[k];
        if (!c->have_first) { c->have_first = true; c->last_us = now; return; }
        uint32_t d = now - c->last_us;
        c->last_us = now;
        if (c->n < RX_MAX_EDGES) c->buf[c->n++] = (d > 65535u) ? 65535u : (uint16_t)d;
        return;
    }
}

bool remotes_init(void) {
    if (g_inited) return true;

    // RF OOK on pio1
    if (!pio_can_add_program(rf_pio, &rf_ook_program)) {
        printf("[remotes] cannot add rf_ook program\n");
        return false;
    }
    rf_offset = pio_add_program(rf_pio, &rf_ook_program);
    rf_sm = (uint)pio_claim_unused_sm(rf_pio, true);
    rf_ook_program_init(rf_pio, rf_sm, rf_offset, RF_TX_PIN);

    // IR on pio0 at the default carrier
    if (!pio_can_add_program(ir_pio, &ir_tx_program)) {
        printf("[remotes] cannot add ir_tx program\n");
        return false;
    }
    ir_offset = pio_add_program(ir_pio, &ir_tx_program);
    ir_sm = (uint)pio_claim_unused_sm(ir_pio, true);
    ir_tx_program_init(ir_pio, ir_sm, ir_offset, IR_TX_PIN, IR_CARRIER_HZ);
    ir_carrier_loaded = IR_CARRIER_HZ;

    // Receive ("learn") inputs. The modules drive these lines; we only enable
    // the per-pin edge IRQ while a channel is armed. The callback is shared by
    // all GPIO IRQs on this core (capture runs on core 0).
    gpio_init(IR_RX_PIN); gpio_set_dir(IR_RX_PIN, GPIO_IN); gpio_pull_up(IR_RX_PIN); // TSOP idles HIGH
    gpio_init(RF_RX_PIN); gpio_set_dir(RF_RX_PIN, GPIO_IN);
#if defined(IR_RX_PWR_PIN) && (IR_RX_PWR_PIN) >= 0
    // Power the IR receiver from a GPIO held HIGH so it can plug in jumperless.
    gpio_init(IR_RX_PWR_PIN);
    gpio_set_dir(IR_RX_PWR_PIN, GPIO_OUT);
    gpio_put(IR_RX_PWR_PIN, 1);
    sleep_ms(50);   // let the receiver's supply settle / AGC wake before arming
#endif
    // Register the shared callback. We pass enabled=true so the GPIO bank IRQ
    // is guaranteed to be enabled in the NVIC (some SDK paths only enable
    // IO_IRQ_BANK0 when the call's `enabled` flag is set — registering with
    // false leaves the line off, so the ISR never fires even though edges latch
    // in the GPIO status). We then immediately disable the per-pin event so
    // capture stays idle until a channel is armed, and assert the bank IRQ
    // explicitly as belt-and-suspenders.
    gpio_set_irq_enabled_with_callback(IR_RX_PIN,
        GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, true, rx_gpio_isr);
    gpio_set_irq_enabled(IR_RX_PIN, GPIO_IRQ_EDGE_RISE | GPIO_IRQ_EDGE_FALL, false);
    irq_set_enabled(IO_IRQ_BANK0, true);

    g_inited = true;
    printf("[remotes] init ok — IR on GPIO%d (%.1f kHz), RF on GPIO%d\n",
           IR_TX_PIN, IR_CARRIER_HZ / 1000.0, RF_TX_PIN);
#if IR_RX_PWR_PIN >= 0
    printf("[remotes] IR RX on GPIO%d, powered from GPIO%d\n",
           IR_RX_PIN, IR_RX_PWR_PIN);
#else
    printf("[remotes] IR RX on GPIO%d, powered from the 3V3 rail\n", IR_RX_PIN);
#endif
    return true;
}

// Reconfigure the IR SM's clock divider if a frame asks for a different carrier.
static void ir_set_carrier(int carrier_hz) {
    if (carrier_hz <= 0) carrier_hz = IR_CARRIER_HZ;
    if (carrier_hz == ir_carrier_loaded) return;
    pio_sm_set_enabled(ir_pio, ir_sm, false);
    float div = (float)clock_get_hz(clk_sys) / ((float)carrier_hz * 4.0f);
    pio_sm_set_clkdiv(ir_pio, ir_sm, div);
    pio_sm_clkdiv_restart(ir_pio, ir_sm);
    pio_sm_set_enabled(ir_pio, ir_sm, true);
    ir_carrier_loaded = carrier_hz;
}

void ir_send_raw(const uint16_t *timings_us, int count, int carrier_hz) {
    if (!g_inited || !timings_us || count <= 0) return;
    if (carrier_hz <= 0) carrier_hz = IR_CARRIER_HZ;
    ir_set_carrier(carrier_hz);

    // Convert each microsecond duration to a count of carrier PERIODS, since the
    // PIO program counts whole carrier periods. periods = us * fc / 1e6.
    // Feed as pairs (mark_periods, space_periods). If the array ends on a mark,
    // emit a 0-length space to complete the pair.
    const double us_to_periods = (double)carrier_hz / 1000000.0;
    for (int i = 0; i < count; i += 2) {
        uint32_t mark  = (uint32_t)(timings_us[i] * us_to_periods + 0.5);
        uint32_t space = 0;
        if (i + 1 < count) space = (uint32_t)(timings_us[i + 1] * us_to_periods + 0.5);
        // The mark_loop runs (X+1) periods because of jmp x-- semantics, so
        // subtract 1 from non-zero counts to match the requested duration.
        pio_sm_put_blocking(ir_pio, ir_sm, mark  ? mark  - 1 : 0);
        pio_sm_put_blocking(ir_pio, ir_sm, space ? space - 1 : 0);
    }
    // Let the FIFO drain so back-to-back sends don't overlap.
    while (!pio_sm_is_tx_fifo_empty(ir_pio, ir_sm)) tight_loop_contents();
    sleep_us(200);
}

// Standard NEC: 9 ms lead mark, 4.5 ms space, then 32 bits LSB-first where a
// '0' = 560us mark + 560us space and a '1' = 560us mark + 1690us space, then a
// final 560us stop mark. Builds the raw timing array and sends at 38 kHz.
void ir_send_nec(uint32_t nec_code) {
    uint16_t t[4 + 32 * 2 + 1];
    int n = 0;
    t[n++] = 9000;   // lead mark
    t[n++] = 4500;   // lead space
    for (int b = 0; b < 32; b++) {
        uint32_t bit = (nec_code >> b) & 1u;   // LSB first
        t[n++] = 560;
        t[n++] = bit ? 1690 : 560;
    }
    t[n++] = 560;    // stop mark
    ir_send_raw(t, n, 38000);
}

void rf_send_raw(const uint16_t *timings_us, int count, int repeat) {
    if (!g_inited || !timings_us || count <= 0) return;
    if (repeat < 1) repeat = 1;
    if (repeat > 50) repeat = 50;

    // The SM consumes words in strict mark,gap,mark,gap order (level alternates
    // starting HIGH). To stay in sync we must always feed an EVEN number of
    // words per frame. 1 tick == 1 us; loops run (X+1) ticks so we subtract 1.
    const uint32_t GAP_US = 10000;   // ~10 ms inter-frame gap (low)
    for (int r = 0; r < repeat; r++) {
        int i = 0;
        for (; i + 1 < count; i += 2) {
            uint32_t mark = timings_us[i];
            uint32_t gap  = timings_us[i + 1];
            pio_sm_put_blocking(rf_pio, rf_sm, mark ? mark - 1 : 0);
            pio_sm_put_blocking(rf_pio, rf_sm, gap  ? gap  - 1 : 0);
        }
        if (i < count) {
            // Odd-length array: final lone mark, pair it with the inter-frame gap.
            uint32_t mark = timings_us[i];
            pio_sm_put_blocking(rf_pio, rf_sm, mark ? mark - 1 : 0);
            pio_sm_put_blocking(rf_pio, rf_sm, GAP_US - 1);
        } else {
            // Even array: append an explicit (0 mark, gap) pair as separation.
            pio_sm_put_blocking(rf_pio, rf_sm, 0);
            pio_sm_put_blocking(rf_pio, rf_sm, GAP_US - 1);
        }
    }
    while (!pio_sm_is_tx_fifo_empty(rf_pio, rf_sm)) tight_loop_contents();
    sleep_us(500);
}

// ── Receive: arm / poll / fetch / decode ─────────────────────────────────────
void remotes_rx_arm(rx_kind_t kind) {
    int k = (int)kind;
    rx_chan_t *c = &g_rx[k];
    c->armed = false;                 // freeze ISR while we reset
    c->n = 0; c->have_first = false; c->ready = false; c->result_n = 0;
    c->arm_deadline = time_us_32() + 12u * 1000u * 1000u;   // 12 s to press a button
    c->armed = true;
    rx_irq_set(k, true);
}

bool remotes_rx_armed(rx_kind_t kind) { return g_rx[(int)kind].armed; }
int  remotes_rx_edges(rx_kind_t kind) { return g_rx[(int)kind].n; }

// Diagnostic: tight-poll the IR RX pin for `ms` with NO interrupt involved,
// counting level transitions and noting whether it was ever pulled LOW. Lets us
// tell "the ISR isn't firing" apart from "the pin sees nothing". Returns the
// idle level at the end (TSOP/VS1838B idles HIGH = 1).
int remotes_ir_raw_sample(int ms, int *transitions, int *low_seen) {
    int last = gpio_get(IR_RX_PIN), tr = 0, low = 0;
    absolute_time_t end = make_timeout_time_ms(ms);
    while (!time_reached(end)) {
        int v = gpio_get(IR_RX_PIN);
        if (v != last) { tr++; last = v; }
        if (!v) low = 1;
    }
    if (transitions) *transitions = tr;
    if (low_seen)    *low_seen    = low;
    return gpio_get(IR_RX_PIN);
}

void remotes_rx_poll(void) {
    uint32_t now = time_us_32();
    for (int k = 0; k < 2; k++) {
        rx_chan_t *c = &g_rx[k];
        if (!c->armed) continue;

        // Overall timeout: give up if no usable frame within the arm window.
        if ((int32_t)(now - c->arm_deadline) > 0) {
            c->armed = false; rx_irq_set(k, false); continue;
        }

        bool finalize = (c->n >= RX_MAX_EDGES) ||
                        (c->have_first && c->n > 0 && (now - c->last_us) > RX_GAP_US[k]);
        if (!finalize) continue;

        rx_irq_set(k, false);                       // freeze while we snapshot
        int n = c->n; if (n > RX_MAX_EDGES) n = RX_MAX_EDGES;

        bool ok = (n >= RX_MIN_EDGES[k]);
        if (ok && k == (int)RX_RF) {                // noise filter: need a real sync gap
            bool sync = false;
            for (int i = 0; i < n; i++) if (c->buf[i] >= RF_SYNC_MIN_US) { sync = true; break; }
            ok = sync;
        }

        if (ok) {
            for (int i = 0; i < n; i++) c->result[i] = c->buf[i];
            c->result_n = n;
            c->ready = true;
            c->armed = false;                       // one-shot; caller re-arms for the next button
        } else {
            c->n = 0; c->have_first = false;        // discard (noise) and keep listening
            rx_irq_set(k, true);
        }
    }
}

bool remotes_rx_get(rx_kind_t kind, uint16_t *out, int max, int *count) {
    rx_chan_t *c = &g_rx[(int)kind];
    if (!c->ready) return false;
    int n = c->result_n; if (n > max) n = max;
    for (int i = 0; i < n; i++) out[i] = c->result[i];
    if (count) *count = n;
    c->ready = false;
    return true;
}

static bool _near(uint32_t v, uint32_t target, uint32_t tol_pct) {
    uint32_t tol = target * tol_pct / 100;
    return (v + tol >= target) && (v <= target + tol);
}

bool remotes_decode_nec(const uint16_t *t, int n, uint32_t *code_out) {
    if (n < 66) return false;                          // 2 header + 32*2 + (stop optional)
    if (!_near(t[0], 9000, 30) || !_near(t[1], 4500, 30)) return false;
    uint32_t code = 0;
    for (int i = 0; i < 32; i++) {
        uint32_t mark  = t[2 + i * 2];
        uint32_t space = t[3 + i * 2];
        if (!_near(mark, 560, 50)) return false;
        if      (_near(space, 560, 50))  { /* bit 0 */ }
        else if (_near(space, 1690, 40)) { code |= (1u << i); }   // LSB-first
        else return false;
    }
    if (code_out) *code_out = code;
    return true;
}
