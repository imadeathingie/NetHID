# Host-side test of the keyboard pipeline. No Pico, no toolchain, no flashing:
# the GPIO layer and the clock are faked, everything above them is the real
# firmware source. Run it before every flash — it catches tap-hold and merge
# regressions in under a second.
#
#   make && ./kbtest

KB     ?= proto2x2
KEYMAP ?= advanced
ROOT   := ../..

FEATURES := -DKB_FEATURE_LAYERS=1 -DKB_FEATURE_TAPPING=1 -DKB_FEATURE_ONESHOT=1 \
            -DKB_FEATURE_COMBO=1 -DKB_FEATURE_CAPS_WORD=0 -DKB_FEATURE_MACROS=1 \
            -DKB_FEATURE_DYNAMIC_KEYMAP=1 -DKB_FEATURE_MOUSEKEYS=1 -DKB_FEATURE_MACRO_STORE=1

SRC := harness.cpp test.cpp \
       $(ROOT)/src/kb/matrix.cpp $(ROOT)/src/kb/debounce.cpp \
       $(ROOT)/src/kb/keystate.cpp $(ROOT)/src/kb/keyboard.cpp \
       $(ROOT)/src/kb/features/layers.cpp $(ROOT)/src/kb/features/tapping.cpp \
       $(ROOT)/src/kb/features/oneshot.cpp $(ROOT)/src/kb/features/combo.cpp \
       $(ROOT)/src/kb/features/macros.cpp $(ROOT)/src/kb/features/mousekeys.cpp \
       $(ROOT)/src/kb/keymap_store.cpp $(ROOT)/src/kb/macro_store.cpp \
       $(ROOT)/src/kb/encoder.cpp \
       $(ROOT)/keyboards/$(KB)/keymaps/$(KEYMAP)/keymap.cpp

kbtest: $(SRC)
	g++ -std=c++17 -O0 -g -Wall -Wextra \
	    -Istubs -I$(ROOT)/include -I$(ROOT)/keyboards/$(KB) \
	    $(FEATURES) -DENABLE_KEYBOARD=1 $(SRC) -o $@

# Standalone check of a board's LAYOUT() macro. Needs nothing but keyboard.h.
layout_check: layout_check.cpp $(ROOT)/keyboards/$(KB)/keyboard.h
	g++ -std=c++17 -Wall -Wextra -Istubs -I$(ROOT)/include \
	    -I$(ROOT)/keyboards/$(KB) layout_check.cpp -o $@

# Settings store: defaults, ranges, override tracking, persistence.
settings_test: settings_test.cpp harness.cpp $(ROOT)/src/settings.cpp $(ROOT)/src/hid_layout.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include \
	    -I$(ROOT)/keyboards/$(KB) -DENABLE_SETTINGS=1 -DENABLE_AP_MODE=1 \
	    -DENABLE_KEYBOARD=1 -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp settings_test.cpp $(ROOT)/src/settings.cpp \
	    $(ROOT)/src/hid_layout.cpp -o $@

# What the device will actually type, per layout. Links the real settings store
# so the layout can be switched the way the web UI switches it.
layout_test: layout_test.cpp harness.cpp $(ROOT)/src/hid.cpp $(ROOT)/src/hid_layout.cpp $(ROOT)/src/settings.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include \
	    -I$(ROOT)/keyboards/$(KB) -DENABLE_KEYBOARD=0 -DENABLE_SETTINGS=1 \
	    -DENABLE_AP_MODE=1 -DHARNESS_REAL_HID=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp layout_test.cpp $(ROOT)/src/hid.cpp \
	    $(ROOT)/src/hid_layout.cpp $(ROOT)/src/settings.cpp -o $@

# String typer against a deliberately flaky USB endpoint.
typer_test: typer_test.cpp harness.cpp $(ROOT)/src/hid.cpp $(ROOT)/src/hid_layout.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include \
	    -I$(ROOT)/keyboards/$(KB) -DENABLE_KEYBOARD=0 -DENABLE_SETTINGS=0 -DHARNESS_REAL_HID=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp typer_test.cpp $(ROOT)/src/hid.cpp \
	    $(ROOT)/src/hid_layout.cpp -o $@

# Split link, over a deliberately bad wire. Needs a split board definition.
split_test: split_test.cpp harness.cpp $(ROOT)/src/split/split_uart.cpp $(ROOT)/src/split/split_primary.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include -I$(ROOT)/src \
	    -I$(ROOT)/keyboards/modular -DSPLIT_ENABLE=1 -DENABLE_KEYBOARD=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp split_test.cpp $(ROOT)/src/split/split_uart.cpp \
	    $(ROOT)/src/split/split_primary.cpp $(ROOT)/src/kb/encoder.cpp \
	    $(ROOT)/src/oled/kb_status.cpp -o $@

# Quadrature decoding against a bouncing encoder.
encoder_test: encoder_test.cpp harness.cpp $(ROOT)/src/kb/encoder.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include -I$(ROOT)/src \
	    -I$(ROOT)/keyboards/modular -DENABLE_KEYBOARD=1 -DHARNESS_NO_GPIO_GET=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp encoder_test.cpp $(ROOT)/src/kb/encoder.cpp -o $@

# Encoder maps in the keymap store. Uses oledpad, which has an encoder; the
# main suite's board does not, so the assertion would compile away there.
encmap_test: encmap_test.cpp harness.cpp $(ROOT)/src/kb/keymap_store.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include \
	    -I$(ROOT)/keyboards/oledpad -DENABLE_KEYBOARD=1 \
	    -DKB_FEATURE_DYNAMIC_KEYMAP=1 -DKB_FEATURE_LAYERS=1 -DKB_FEATURE_CONSUMER=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp encmap_test.cpp $(ROOT)/src/kb/keymap_store.cpp \
	    $(ROOT)/keyboards/oledpad/keymaps/default/keymap.cpp -o $@

# Autoclick triggers and rate. Uses oledpad, which declares the slots.
autoclick_test: autoclick_test.cpp harness.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include -I$(ROOT)/src \
	    -I$(ROOT)/keyboards/oledpad -DENABLE_KEYBOARD=1 -DENABLE_SETTINGS=0 \
	    -DKB_FEATURE_LAYERS=1 -DKB_FEATURE_TAPPING=1 -DKB_FEATURE_MOUSEKEYS=1 \
	    -DKB_FEATURE_MACROS=1 -DKB_FEATURE_CONSUMER=1 -DKB_FEATURE_AUTOCLICK=1 \
	    -DKB_FEATURE_DYNAMIC_KEYMAP=1 -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp autoclick_test.cpp $(ROOT)/src/kb/matrix.cpp \
	    $(ROOT)/src/kb/debounce.cpp $(ROOT)/src/kb/keystate.cpp \
	    $(ROOT)/src/kb/keyboard.cpp $(ROOT)/src/kb/encoder.cpp \
	    $(ROOT)/src/kb/keymap_store.cpp $(ROOT)/src/kb/features/layers.cpp \
	    $(ROOT)/src/kb/features/tapping.cpp $(ROOT)/src/kb/features/mousekeys.cpp \
	    $(ROOT)/src/kb/features/macros.cpp $(ROOT)/src/kb/features/consumer.cpp \
	    $(ROOT)/src/kb/features/autoclick.cpp \
	    $(ROOT)/keyboards/oledpad/keymaps/default/keymap.cpp -o $@

# The runtime password override, and the build-id rule that makes flashing
# firmware the way back in after forgetting one.
auth_store_test: auth_store_test.cpp harness.cpp $(ROOT)/src/auth_store.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include -I$(ROOT)/src \
	    -I$(ROOT)/keyboards/$(KB) -DENABLE_PASSWORD_STORE=1 -DENABLE_KEYBOARD=0 \
	    -DENABLE_SETTINGS=0 -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp auth_store_test.cpp $(ROOT)/src/auth_store.cpp -o $@

# OLED framebuffer, font and dirty-page flushing.
oled_test: oled_test.cpp harness.cpp $(ROOT)/src/oled/oled.cpp
	g++ -std=c++17 -O0 -Wall -Wextra -Istubs -I$(ROOT)/include -I$(ROOT)/src \
	    -I$(ROOT)/keyboards/modular -DENABLE_KEYBOARD=1 -DOLED_ENABLE=1 \
	    -DWIFI_NETWORK_LIST='WIFI("a","b",0)' \
	    harness.cpp oled_test.cpp $(ROOT)/src/oled/oled.cpp \
	    $(ROOT)/src/oled/oled_status.cpp $(ROOT)/src/oled/kb_status.cpp -o $@

# Everything that runs on a host.
check: kbtest settings_test layout_test typer_test split_test encoder_test oled_test encmap_test autoclick_test auth_store_test
	./kbtest && ./settings_test && ./typer_test && ./split_test && \
	./encoder_test && ./oled_test && ./encmap_test && ./autoclick_test && \
	./auth_store_test

clean:
	rm -f kbtest layout_check layout_test settings_test typer_test split_test encoder_test encmap_test autoclick_test oled_test
.PHONY: clean check
