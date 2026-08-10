# Features this keyboard wants. kb_default() only sets a value that wasn't
# already given on the command line, so
#
#   cmake .. -DKEYBOARD=proto2x2 -DKB_FEATURE_COMBO=OFF
#
# still wins over anything here.

kb_default(KB_FEATURE_LAYERS    ON)
kb_default(KB_FEATURE_TAPPING   ON)
kb_default(KB_FEATURE_ONESHOT   ON)
kb_default(KB_FEATURE_COMBO     ON)
kb_default(KB_FEATURE_CAPS_WORD OFF)
kb_default(KB_FEATURE_MACROS    ON)
kb_default(KB_FEATURE_DYNAMIC_KEYMAP ON)   # web-UI keymap editing + flash persistence
kb_default(KB_FEATURE_MOUSEKEYS ON)
kb_default(KB_FEATURE_MACRO_STORE ON)     # web-built macro bodies for KB_MACRO(n)
