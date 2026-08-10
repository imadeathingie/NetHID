/* layers.cpp — MO / TO / TG / DF and the layer-stack lookup. */

#include "kb/layers.h"
#include "kb/features.h"

static uint32_t layer_state;      /* bit n = layer n momentarily active */
static uint8_t  default_layer;

void kb_layers_init(void) { layer_state = 0; default_layer = 0; }

void kb_layer_on(uint8_t l)      { if (l < 32) layer_state |=  (1u << l); }
void kb_layer_off(uint8_t l)     { if (l < 32) layer_state &= ~(1u << l); }
void kb_layer_toggle(uint8_t l)  { if (l < 32) layer_state ^=  (1u << l); }
void kb_layer_move(uint8_t l)    { layer_state = (l < 32) ? (1u << l) : 0; }
void kb_layer_set_default(uint8_t l) { if (l < kb_keymap_layers()) default_layer = l; }
bool kb_layer_is_on(uint8_t l)   { return l < 32 && (layer_state & (1u << l)); }
uint32_t kb_layer_state(void)    { return layer_state; }

uint8_t kb_layer_highest(void) {
    for (int l = 31; l >= 0; l--) if (layer_state & (1u << l)) return (uint8_t)l;
    return default_layer;
}

kb_keycode_t kb_layer_lookup(keypos_t pos) {
    for (int l = 31; l >= 0; l--) {
        if (!(layer_state & (1u << l))) continue;
        if (l >= kb_keymap_layers()) continue;
        kb_keycode_t kc = kb_keymap_at((uint8_t)l, pos.row, pos.col);
        if (kc != KC_TRNS) return kc;
    }
    return kb_keymap_at(default_layer, pos.row, pos.col);
}

bool kb_layers_process(keyrecord_t *rec) {
    kb_keycode_t kc = rec->keycode;
    uint8_t l = (uint8_t)(kc & 0xFF);

    switch (kc & 0xFF00) {
    case QK_MOMENTARY:
        if (rec->event.pressed) kb_layer_on(l); else kb_layer_off(l);
        return false;
    case QK_TOGGLE_LAYER:
        if (rec->event.pressed) kb_layer_toggle(l);
        return false;
    case QK_TO:
        if (rec->event.pressed) kb_layer_move(l);
        return false;
    case QK_DEF_LAYER:
        if (rec->event.pressed) kb_layer_set_default(l);
        return false;
    default:
        return true;   /* not ours */
    }
}

void kb_layers_task(void) { }
