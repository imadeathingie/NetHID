/*
 * encoder_test — quadrature decoding against a bouncing, imperfect encoder.
 *
 * The point is not that a clean rotation produces steps; it is that a cheap
 * encoder's mechanical bounce does NOT. Reading one edge and trusting it turns
 * a single click into a burst, and the burst is always in the same direction,
 * so volume jumps ten steps. The transition table exists for that.
 */
#include <stdio.h>
#include <string.h>
#include "kb/encoder.h"

extern uint32_t fake_now_ms;

/* Pin state the fake GPIO layer returns. Indices are the pins in module0.h. */
static bool pin_level[32];
bool gpio_get(uint32_t p) { return pin_level[p]; }

#define PIN_A 26
#define PIN_B 27
#define PIN_SW 28

static int fails = 0;
static void ck(const char *n, bool ok) {
    printf("%-46s %s\n", n, ok ? "PASS" : "FAIL");
    if (!ok) fails++;
}

/* Encoders idle high (pull-ups) and pull low through the detents. */
static void set_ab(int a, int b) {
    pin_level[PIN_A] = a;
    pin_level[PIN_B] = b;
    encoder_task();
}

/*
 * One full detent, as a 4-state Gray sequence. Which physical direction these
 * correspond to depends on which of A and B is soldered where — there is no
 * correct answer, only a convention. These match the firmware's; a board that
 * finds its knob backwards sets ENCODER_REVERSED rather than rewiring.
 */
static void click_cw(void)  { set_ab(0,1); set_ab(0,0); set_ab(1,0); set_ab(1,1); }
static void click_ccw(void) { set_ab(1,0); set_ab(0,0); set_ab(0,1); set_ab(1,1); }

static int drain(int *cw, int *ccw, int *press, int *release) {
    uint8_t i; enc_action_t a; bool p; int n = 0;
    while (encoder_next(&i, &a, &p)) {
        n++;
        if (a == ENC_CW) (*cw)++;
        else if (a == ENC_CCW) (*ccw)++;
        else if (p) (*press)++;
        else (*release)++;
    }
    return n;
}

int main(void) {
    pin_level[PIN_A] = pin_level[PIN_B] = pin_level[PIN_SW] = 1;
    encoder_init();

    int cw = 0, ccw = 0, pr = 0, rl = 0;

    /* 1. One click one way, one the other. */
    click_cw();  drain(&cw, &ccw, &pr, &rl);
    ck("one clockwise click is one step", cw == 1 && ccw == 0);
    click_ccw(); drain(&cw, &ccw, &pr, &rl);
    ck("one anticlockwise click is one step", cw == 1 && ccw == 1);

    /* 2. Ten clicks are ten steps, not nine or eleven. */
    cw = ccw = 0;
    for (int i = 0; i < 10; i++) click_cw();
    drain(&cw, &ccw, &pr, &rl);
    ck("ten clicks are ten steps", cw == 10 && ccw == 0);

    /* 3. Bounce on the A line produces NOTHING. This is the case the table is
     *    for: an edge-triggered decoder reads each of these as a step and sends
     *    a burst, always in one direction. */
    cw = ccw = 0;
    for (int i = 0; i < 20; i++) { set_ab(0,1); set_ab(1,1); }
    drain(&cw, &ccw, &pr, &rl);
    ck("bounce on one line yields no steps", cw == 0 && ccw == 0);

    /* 4. A half-turn that reverses mid-detent nets out to zero. */
    cw = ccw = 0;
    set_ab(1,0); set_ab(0,0); set_ab(1,0); set_ab(1,1);
    drain(&cw, &ccw, &pr, &rl);
    ck("a reversed partial turn nets to zero", cw == 0 && ccw == 0);

    /* 5. The push switch is a real button: press and release, debounced. */
    pr = rl = 0;
    pin_level[PIN_SW] = 0;                 /* pressed */
    fake_now_ms += 20; encoder_task();
    drain(&cw, &ccw, &pr, &rl);
    pin_level[PIN_SW] = 1;                 /* released */
    fake_now_ms += 20; encoder_task();
    drain(&cw, &ccw, &pr, &rl);
    ck("push switch reports press then release", pr == 1 && rl == 1);

    /* 6. Switch bounce inside the debounce window is one press, not five. */
    pr = rl = 0;
    for (int i = 0; i < 5; i++) {
        pin_level[PIN_SW] = 0; encoder_task();
        pin_level[PIN_SW] = 1; encoder_task();
    }
    pin_level[PIN_SW] = 0;
    fake_now_ms += 20; encoder_task();
    drain(&cw, &ccw, &pr, &rl);
    ck("switch bounce is debounced to one press", pr == 1);

    printf("\n%s\n", fails ? "FAILURES" : "all green");
    return fails;
}
