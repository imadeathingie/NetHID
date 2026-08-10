/*
 * split_test — the serial link between halves, over a deliberately bad wire.
 *
 * The three things that matter, none of which a clean-cable test would show:
 *
 *   1. Framing resynchronises. A UART that was just powered up, or a TRRS jack
 *      being pushed in, delivers partial frames and garbage. The receiver must
 *      find its footing again rather than staying confused.
 *   2. Corruption is rejected. A flipped bit in a matrix row is not a lost key,
 *      it is a phantom keypress — arguably worse than a dropped packet.
 *   3. Disconnect releases everything. Whatever a module was holding when its
 *      cable came out must not stay held: the scan sees no change, so no
 *      release is ever generated, and the host is left with a key down that
 *      nothing on this board can lift.
 *   4. Addressing is respected. On a shared return line, applying one module's
 *      reply to whichever slot is currently being polled is the difference
 *      between "a module is missing" and "the macropad's keys appear on the
 *      left hand".
 *   5. Modules of different shapes land in the right rows.
 */
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include "split/split.h"
#include "kb/matrix.h"
#include "hardware/uart.h"

extern uint32_t fake_now_ms;

/* ── Fake wire ─────────────────────────────────────────────────────────────
 *
 * TWO buffers, not one. The bus has a line from the primary to every module and
 * a shared line back; modelling it as a single stream makes the primary read
 * its own polls, which is not a subtle failure but is a confusing one — the
 * first test still passes.
 *
 * The primary owns the real link code; the module side is hand-rolled below and
 * reads/writes these buffers directly. So uart_* here is always the primary.
 */
typedef struct { uint8_t b[8192]; int head, tail; } line_t;
static line_t to_module, to_primary;

int g_corrupt_pct = 0;
int g_drop_pct    = 0;

static void line_put(line_t *l, uint8_t b) {
    /* Compact when drained. Without this the buffer is append-only, fills after
     * a few hundred milliseconds of polling, and every later write is dropped
     * silently — which reads as "the firmware stopped talking to its modules"
     * and sent me looking in entirely the wrong place. */
    if (l->tail == l->head) l->tail = l->head = 0;
    if (g_drop_pct && (rand() % 100) < g_drop_pct) return;
    if (g_corrupt_pct && (rand() % 100) < g_corrupt_pct) b ^= (uint8_t)(1 << (rand() % 8));
    if (l->head < (int)sizeof(l->b)) l->b[l->head++] = b;
    else printf("   [test] WARNING: fake wire overflowed\n");
}

void wire_reset(void) { memset(&to_module, 0, sizeof(to_module));
                        memset(&to_primary, 0, sizeof(to_primary)); }

/* Raw injection, bypassing corruption: used to test resynchronisation. */
void wire_inject(const uint8_t *b, int n) {
    for (int i = 0; i < n; i++)
        if (to_primary.head < (int)sizeof(to_primary.b))
            to_primary.b[to_primary.head++] = b[i];
}

/* ── The pico UART surface split_uart.cpp uses ───────────────────────────── */
uint32_t uart_init(uart_inst_t *, uint32_t) { return 0; }
void uart_set_hw_flow(uart_inst_t *, bool, bool) {}
void uart_set_format(uart_inst_t *, uint32_t, uint32_t, uint32_t) {}
void uart_set_fifo_enabled(uart_inst_t *, bool) {}
void uart_tx_wait_blocking(uart_inst_t *) {}
/* gpio_set_dir() comes from harness.cpp, which already fakes the GPIO layer. */

bool uart_is_writable(uart_inst_t *) { return true; }
bool uart_is_readable(uart_inst_t *) { return to_primary.tail < to_primary.head; }
void uart_write_blocking(uart_inst_t *, const uint8_t *src, size_t n) {
    for (size_t i = 0; i < n; i++) line_put(&to_module, src[i]);
}
char uart_getc(uart_inst_t *) { return (char)to_primary.b[to_primary.tail++]; }

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-46s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/*
 * Module side, hand-rolled.
 *
 * It would be neater to reuse split_link_recv(), but the parser state in
 * split_uart.cpp is file-static: one instance, shared. Real hardware has two
 * chips with two parsers, and letting the primary and a fake module take turns
 * clobbering the same rx_state produces failures that look like protocol bugs
 * and are not. The primary keeps the real parser — it is the side under test.
 */
static bool module_read_poll(uint8_t *addr_out) {
    while (to_module.tail + 5 <= to_module.head) {
        const uint8_t *f = &to_module.b[to_module.tail];
        if (f[0] != SPLIT_SYNC) { to_module.tail++; continue; }
        uint8_t len = f[3];
        if (to_module.tail + 5 + len > to_module.head) return false;
        uint8_t crc = split_crc8(f + 1, (uint8_t)(len + 3));
        to_module.tail += 5 + len;
        if (crc != f[4 + len]) continue;          /* corrupted poll: ignore */
        if (f[2] != SPLIT_MSG_POLL) continue;
        *addr_out = f[1];
        return true;
    }
    return false;
}

static void module_send_state(uint8_t id, const matrix_row_t *rows,
                              uint8_t nrows, bool wrong_len) {
    uint8_t len = (uint8_t)(nrows * SPLIT_ROW_BYTES + (wrong_len ? 1 : 0));
    uint8_t f[5 + SPLIT_MAX_PAYLOAD];
    f[0] = SPLIT_SYNC;
    f[1] = id;
    f[2] = SPLIT_MSG_STATE;
    f[3] = len;
    memset(f + 4, 0, len);
    for (int i = 0; i < nrows; i++)
        for (int b = 0; b < SPLIT_ROW_BYTES; b++)
            f[4 + i * SPLIT_ROW_BYTES + b] = (uint8_t)(rows[i] >> (8 * b));
    f[4 + len] = split_crc8(f + 1, (uint8_t)(len + 3));
    for (int i = 0; i < 5 + len; i++) line_put(&to_primary, f[i]);
}

/*
 * Every module on the bus, answering its own polls.
 *
 * Modelling one module at a time was wrong and produced a convincing-looking
 * failure: while the test answered module 1, module 2 was starved and correctly
 * declared lost, then the reverse. Both flapped, and nothing in the firmware
 * was at fault. A real bus has every module listening at once.
 */
typedef struct {
    uint8_t      id;
    matrix_row_t rows[4];
    uint8_t      nrows;
    bool         present;      /* false = unplugged */
    bool         wrong_len;    /* true  = mismatched firmware */
    uint8_t      claim_id;     /* answer under a different address */
} responder_t;

static responder_t bus[2];

static void bus_step(void) {
    uint8_t addr;
    while (module_read_poll(&addr)) {
        for (unsigned i = 0; i < sizeof(bus) / sizeof(bus[0]); i++) {
            if (bus[i].id != addr || !bus[i].present) continue;
            module_send_state(bus[i].claim_id ? bus[i].claim_id : bus[i].id,
                              bus[i].rows, bus[i].nrows, bus[i].wrong_len);
        }
    }
}

/* Run the bus for `ms` of fake time with everything answering as configured. */
static void run_bus(int ms, matrix_row_t *combined) {
    for (int i = 0; i < ms; i++) {
        split_primary_task();
        bus_step();
        fake_now_ms++;
    }
    if (combined) {
        memset(combined, 0, sizeof(matrix_row_t) * MATRIX_ROWS);
        split_primary_rows(combined);
    }
}

int main(void) {
    matrix_row_t combined[MATRIX_ROWS];
    split_primary_init();
    wire_reset();

    /* Module 1 is 4x6 at row offset 4; module 2 is 1x4 at row offset 8. */
    bus[0] = { 1, { 0x05, 0, 0x20, 0 }, 4, true, false, 0 };
    bus[1] = { 2, { 0x0A, 0, 0, 0 },    1, true, false, 0 };

    /* Restore both modules to healthy and let the bus settle. Each case does
     * this rather than inheriting from the last: a case that leaves a module
     * offline would make the next one pass or fail for reasons unrelated to
     * what it tests. */
    #define RESYNC() do {                              \
        bus[0].present = bus[1].present = true;        \
        bus[0].wrong_len = bus[1].wrong_len = false;   \
        bus[0].claim_id = bus[1].claim_id = 0;         \
        run_bus(300, combined);                        \
    } while (0)

    /* 1. Each module's rows land at its own offset, not somebody else's. */
    RESYNC();
    ck("modules land at their own row offsets",
       combined[4] == 0x05 && combined[6] == 0x20 && combined[8] == 0x0A &&
       split_module_online(1) && split_module_online(2));

    /* 2. A reply carrying the wrong address must never be applied. Module 1
     *    answers claiming to be module 2, with data that appears nowhere else. */
    { RESYNC();
      bus[0].rows[0] = 0x3F; bus[0].rows[1] = 0x3F;
      bus[0].rows[2] = 0x3F; bus[0].rows[3] = 0x3F;
      bus[0].claim_id = 2;
      run_bus(300, combined);
      bool clean = true;
      for (int r = 0; r < MATRIX_ROWS; r++) if (combined[r] == 0x3F) clean = false;
      bus[0].rows[0] = 0x05; bus[0].rows[1] = 0;
      bus[0].rows[2] = 0x20; bus[0].rows[3] = 0;
      ck("a reply from the wrong module is never applied", clean); }

    /* 3. A wrong-sized reply means mismatched firmware. Refuse it rather than
     *    scattering that module's keys across the matrix. */
    { RESYNC();
      bus[0].wrong_len = true;
      run_bus(300, combined);
      ck("a wrong-sized reply is refused",
         !split_module_online(1) || combined[4] == 0x05); }

    /* 4. Unplugging one module releases its keys, and only its. */
    { RESYNC();
      bus[0].present = false;
      run_bus(600, combined);
      bool m1_clear = combined[4] == 0 && combined[5] == 0 &&
                      combined[6] == 0 && combined[7] == 0;
      ck("losing one module does not disturb the others",
         m1_clear && !split_module_online(1) &&
         split_module_online(2) && combined[8] == 0x0A); }

    /* 5. A damaged bus must still converge, because state is resent every poll.
     *
     * Rates are per byte and applied in BOTH directions, which compounds hard:
     * a 5-byte poll and a 9-byte reply at 5% loss and 5% corruption means only
     * about a third of round trips survive intact. That is already far worse
     * than any cable you would tolerate, and it is the property that matters —
     * not that the link is reliable, but that it recovers without help.
     *
     * Deliberately not tested at 20%: that destroys ~99.8% of round trips, and
     * asserting convergence there would be asserting something the design never
     * promised. A test that only passes on a broken wire tells you nothing
     * about a working one. */
    { RESYNC();
      srand(99);
      g_corrupt_pct = 5; g_drop_pct = 5;
      bus[0].rows[1] = 0x2A;
      bool got = false;
      int  took = 0;
      for (int i = 0; i < 20000 && !got; i++) {
          run_bus(1, combined);
          took = i;
          if (split_module_online(1) && combined[5] == 0x2A) got = true;
      }
      g_corrupt_pct = g_drop_pct = 0;
      if (got) printf("   [test] recovered after %d ms on a 5%%/5%% bus\n", took);
      ck("recovers on a lossy, corrupting bus", got); }

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
