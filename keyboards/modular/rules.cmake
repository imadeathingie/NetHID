# The split bus needs uart1, and its only usable pin pair here is GP20/GP21 —
# GP24/25 belong to the CYW43 and everything lower is matrix. GP21 is IR_RX_PIN
# in include/config.h, so this board cannot have both the bus and the IR
# receiver. A modular keyboard is not usually also a remote control, so the
# remotes lose. Run `python3 tools/check_pins.py` after changing any of this.
set(ENABLE_REMOTES OFF CACHE BOOL "" FORCE)

kb_default(KB_FEATURE_LAYERS         ON)
kb_default(KB_FEATURE_TAPPING        ON)
kb_default(KB_FEATURE_ONESHOT        ON)
kb_default(KB_FEATURE_COMBO          ON)
kb_default(KB_FEATURE_CAPS_WORD      ON)
kb_default(KB_FEATURE_MACROS         ON)
kb_default(KB_FEATURE_MACRO_STORE    ON)
kb_default(KB_FEATURE_MOUSEKEYS      ON)
kb_default(KB_FEATURE_BOOTMAGIC      ON)
kb_default(KB_FEATURE_DYNAMIC_KEYMAP ON)
kb_default(KB_FEATURE_CONSUMER ON)
