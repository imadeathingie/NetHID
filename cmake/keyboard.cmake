# ── Physical keyboard support ────────────────────────────────────────────────
#
#   cmake .. -DPICO_BOARD=pico2_w -DKEYBOARD=proto2x2 -DKEYMAP=advanced
#
# Leaving KEYBOARD empty builds NetHID exactly as before — no matrix code is
# compiled and no GPIO is claimed.
#
# Feature selection is three-layered, highest priority first:
#   1. -DKB_FEATURE_<X>=ON/OFF on the command line
#   2. keyboards/<KEYBOARD>/rules.cmake
#   3. OFF
#
# Each feature maps to exactly one source file. Turning it off means that file
# is not compiled, is not linked, and drops out of the dispatch table in
# include/kb/features.h — no dead branch is left behind.

set(KEYBOARD "" CACHE STRING "Directory under keyboards/ to build; empty = network-only")
set(KEYMAP   "default" CACHE STRING "Keymap under keyboards/<KEYBOARD>/keymaps/")

# Only sets a variable that the command line didn't already provide.
macro(kb_default name value)
    if(NOT DEFINED ${name})
        set(${name} ${value})
    endif()
endmacro()

# BOOTMAGIC is in this list so it gets the same on/off handling, but it is NOT
# part of the process chain in include/kb/features.h — it hooks boot and a
# single keycode instead of key events.
set(KB_ALL_FEATURES LAYERS TAPPING ONESHOT COMBO CAPS_WORD MACROS MOUSEKEYS CONSUMER AUTOCLICK BOOTMAGIC DYNAMIC_KEYMAP MACRO_STORE)

# feature name -> source file
set(KB_SRC_LAYERS    src/kb/features/layers.cpp)
set(KB_SRC_TAPPING   src/kb/features/tapping.cpp)
set(KB_SRC_ONESHOT   src/kb/features/oneshot.cpp)
set(KB_SRC_COMBO     src/kb/features/combo.cpp)
set(KB_SRC_CAPS_WORD src/kb/features/caps_word.cpp)
set(KB_SRC_MACROS    src/kb/features/macros.cpp)
set(KB_SRC_MOUSEKEYS src/kb/features/mousekeys.cpp)
set(KB_SRC_CONSUMER  src/kb/features/consumer.cpp)
set(KB_SRC_AUTOCLICK src/kb/features/autoclick.cpp)
set(KB_SRC_BOOTMAGIC src/kb/bootmagic.cpp)
set(KB_SRC_DYNAMIC_KEYMAP src/kb/keymap_store.cpp src/kb/layout.cpp)
set(KB_SRC_MACRO_STORE src/kb/macro_store.cpp)

if(NOT KEYBOARD STREQUAL "")
    set(KB_DIR     ${CMAKE_CURRENT_SOURCE_DIR}/keyboards/${KEYBOARD})
    set(KEYMAP_DIR ${KB_DIR}/keymaps/${KEYMAP})

    if(NOT EXISTS ${KB_DIR}/keyboard.h)
        message(FATAL_ERROR "No keyboards/${KEYBOARD}/keyboard.h — check -DKEYBOARD=")
    endif()
    if(NOT EXISTS ${KEYMAP_DIR}/keymap.cpp)
        message(FATAL_ERROR "No keyboards/${KEYBOARD}/keymaps/${KEYMAP}/keymap.cpp — check -DKEYMAP=")
    endif()

    # A board declares itself split by defining MATRIX_ROWS_PER_SIDE. Detected
    # by reading keyboard.h rather than by another flag to set: the split-ness
    # of a board is a property of its wiring, not a build-time preference, and a
    # mismatch between the two would show up as a half-scanned matrix.
    file(READ ${KB_DIR}/keyboard.h _kb_header)
    if(_kb_header MATCHES "#[ \t]*define[ \t]+SPLIT_MODULES")
        set(SPLIT_ENABLE ON)
    else()
        set(SPLIT_ENABLE OFF)
    endif()

    include(${KB_DIR}/rules.cmake OPTIONAL)
    foreach(f ${KB_ALL_FEATURES})
        kb_default(KB_FEATURE_${f} OFF)
    endforeach()

    # MACRO_STORE is the bytecode body behind KB_MACRO(n); without the feature
    # that dispatches those keycodes it has nothing to attach to.
    if(KB_FEATURE_MACRO_STORE AND NOT KB_FEATURE_MACROS)
        message(FATAL_ERROR "KB_FEATURE_MACRO_STORE needs KB_FEATURE_MACROS=ON")
    endif()

    # AUTOCLICK with no slots compiles to the no-op stub at the bottom of
    # autoclick.cpp: AUTOCLK(n) is then a keycode the web editor happily accepts,
    # stores, and shows on the key — that does nothing when pressed, with no
    # error anywhere. Refuse the combination instead of shipping a dead key.
    if(KB_FEATURE_AUTOCLICK AND NOT _kb_header MATCHES "#[ \t]*define[ \t]+NUM_AUTOCLICKS")
        message(FATAL_ERROR
            "KB_FEATURE_AUTOCLICK=ON but ${KB_DIR}/keyboard.h defines no "
            "NUM_AUTOCLICKS/AUTOCLICKS slots — AUTOCLK(n) would silently do "
            "nothing. Declare slots (see include/kb/autoclick.h) or set the "
            "feature OFF.")
    endif()

    target_sources(nethid PRIVATE
        src/kb/matrix.cpp
        src/kb/debounce.cpp
        src/kb/keystate.cpp
        src/kb/keyboard.cpp
        src/kb/encoder.cpp
        ${KEYMAP_DIR}/keymap.cpp
    )
    target_include_directories(nethid PRIVATE ${KB_DIR} ${KEYMAP_DIR})
    target_compile_definitions(nethid PRIVATE ENABLE_KEYBOARD=1)

    set(_kb_on "")
    foreach(f ${KB_ALL_FEATURES})
        if(KB_FEATURE_${f})
            target_sources(nethid PRIVATE ${KB_SRC_${f}})
            target_compile_definitions(nethid PRIVATE KB_FEATURE_${f}=1)
            list(APPEND _kb_on ${f})
        else()
            target_compile_definitions(nethid PRIVATE KB_FEATURE_${f}=0)
        endif()
    endforeach()

    if(KB_FEATURE_BOOTMAGIC)
        target_link_libraries(nethid pico_bootrom)
    elseif(ENABLE_AP_MODE)
        # AP mode's boot trigger calls kb_matrix_held_at_boot(), which lives in
        # bootmagic.cpp — so a board with AP mode and KB_FEATURE_BOOTMAGIC=OFF
        # failed to LINK, not to compile. Nothing caught it because no default
        # board had that combination; proto2x2 does. Pull the file in for the
        # one function rather than silently dropping the documented AP boot key.
        target_sources(nethid PRIVATE ${KB_SRC_BOOTMAGIC})
        target_link_libraries(nethid pico_bootrom)
    endif()
    if(KB_FEATURE_MACRO_STORE)
        target_link_libraries(nethid hardware_flash hardware_sync pico_multicore)
    endif()
    if(KB_FEATURE_AUTOCLICK)
        # Slots are runtime-editable and persist to flash (5th sector from the end).
        target_link_libraries(nethid hardware_flash hardware_sync pico_multicore)
    endif()
    if(KB_FEATURE_DYNAMIC_KEYMAP)
        # hardware_flash for the erase/program, pico_multicore for the lockout
        # that parks core 1 in RAM while XIP is down.
        target_link_libraries(nethid hardware_flash hardware_sync pico_multicore)
    endif()

    # ── OLED ─────────────────────────────────────────────────────────────────
    # A board opts in by defining OLED_ENABLE in keyboard.h (or a module header,
    # so only the modules that actually have a panel pay for one).
    # The primary is module 0, so its display is declared in modules/module0.h
    # on a modular board and in keyboard.h otherwise. Check both.
    set(_oled_line "")
    file(STRINGS ${KB_DIR}/keyboard.h _oled_kb REGEX "define[ \t]+OLED_ENABLE[ \t]+1")
    if(EXISTS ${KB_DIR}/modules/module0.h)
        file(STRINGS ${KB_DIR}/modules/module0.h _oled_m0 REGEX "define[ \t]+OLED_ENABLE[ \t]+1")
    endif()
    if(_oled_kb OR _oled_m0)
        set(_oled_line ON)
    endif()
    # OLED_ENABLE is passed to the COMPILER, not left to the board header alone.
    # oled.h defaults it to 0 when undefined, and oled.cpp/oled_status.cpp
    # include oled.h before anything pulls in keyboard.h — so the header's
    # `#define OLED_ENABLE 1` arrived too late and every `#if OLED_ENABLE` in
    # the driver was false. The whole driver compiled to nothing on every board,
    # and main.cpp (which sees keyboard.h before its call site but after its
    # include block) failed with the functions undeclared.
    #
    # A macro that decides whether a file has any contents must not depend on
    # include order. Same treatment as KB_FEATURE_* and ENABLE_WEB.
    if(_oled_line OR KB_OLED)
        target_sources(nethid PRIVATE
            src/oled/oled.cpp src/oled/oled_status.cpp src/oled/kb_status.cpp)
        target_link_libraries(nethid hardware_i2c)
        target_compile_definitions(nethid PRIVATE OLED_ENABLE=1)
        message(STATUS "NetHID: OLED status display")
    else()
        # kb_status.cpp is always built: split_primary.cpp publishes through it
        # whether or not this board has a panel, because a MODULE may have one.
        target_sources(nethid PRIVATE src/oled/kb_status.cpp)
        target_compile_definitions(nethid PRIVATE OLED_ENABLE=0)
    endif()

    # ── Split ────────────────────────────────────────────────────────────────
    # A board opts in by defining MATRIX_ROWS_PER_SIDE in keyboard.h. The
    # primary gets the link code folded into the normal firmware; the secondary
    # is a SEPARATE, much smaller executable built from the same board files.
    if(SPLIT_ENABLE)
        target_sources(nethid PRIVATE
            src/split/split_uart.cpp
            src/split/split_primary.cpp
        )
        # The primary is module 0 and scans module 0's pins.
        target_compile_definitions(nethid PRIVATE SPLIT_MODULE_ID=0)
        target_link_libraries(nethid hardware_uart)
        target_compile_definitions(nethid PRIVATE SPLIT_ENABLE=1)
        message(STATUS "NetHID: modular primary; build each module with -DSPLIT_MODULE=<id>")
    else()
        target_compile_definitions(nethid PRIVATE SPLIT_ENABLE=0)
    endif()

    # The web UI reports which board it is editing.
    target_compile_definitions(nethid PRIVATE KB_BOARD_NAME=\"${KEYBOARD}\")

    # combo.cpp uses kb_dispatch_after() to replay deferred events into the
    # rest of the chain; harmless but pointless if it is the only feature.
    # ── Modules ──────────────────────────────────────────────────────────────
    # One executable per module id, each built from the SAME board directory but
    # picking up its own pins from modules/module<id>.h. They share nothing with
    # the primary but the board definition and the matrix scanner: no TinyUSB,
    # no lwIP, no cyw43 and no pico_cyw43_arch link line, which is what lets a
    # module run on a plain Pico or Pico 2 rather than requiring a W.
    #
    #   cmake .. -DKEYBOARD=modular -DSPLIT_MODULE=1
    #   make nethid_module1
    #
    # Every module id in the board's SPLIT_MODULES list except 0 gets a target,
    # so `make` alone builds the primary and nothing else — a module is only
    # built when you ask for it, and the ids come from the same list the primary
    # polls, so the two cannot drift apart.
    if(SPLIT_ENABLE)
        string(REGEX MATCHALL "SPLIT_MODULE\\([ \t]*([0-9]+)" _mods "${_kb_header}")
        foreach(_m ${_mods})
            string(REGEX REPLACE "SPLIT_MODULE\\([ \t]*" "" _id "${_m}")
            if(NOT _id EQUAL 0)
                add_executable(nethid_module${_id}
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/split/main_module.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/split/split_module.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/split/split_uart.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/kb/matrix.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/kb/encoder.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/oled/oled.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/oled/oled_status.cpp
                    ${CMAKE_CURRENT_SOURCE_DIR}/src/oled/kb_status.cpp
                )
                target_include_directories(nethid_module${_id} PRIVATE
                    ${CMAKE_CURRENT_SOURCE_DIR}/include
                    ${CMAKE_CURRENT_SOURCE_DIR}/src
                    ${KB_DIR}
                )
                # Each module's panel is declared in its OWN header, so the
                # value differs per target — module 1 has no display, module 2
                # does. Detected the same way as the primary's, and passed for
                # the same reason: the driver's `#if OLED_ENABLE` must not
                # depend on which header a translation unit happens to include
                # first.
                set(_oled_mod OFF)
                if(EXISTS ${KB_DIR}/modules/module${_id}.h)
                    file(STRINGS ${KB_DIR}/modules/module${_id}.h _oled_m
                         REGEX "define[ \t]+OLED_ENABLE[ \t]+1")
                    if(_oled_m)
                        set(_oled_mod ON)
                    endif()
                endif()
                target_compile_definitions(nethid_module${_id} PRIVATE
                    SPLIT_ENABLE=1
                    SPLIT_MODULE_ID=${_id}
                    ENABLE_KEYBOARD=1
                    $<$<BOOL:${_oled_mod}>:OLED_ENABLE=1>
                    $<$<NOT:$<BOOL:${_oled_mod}>>:OLED_ENABLE=0>
                )
                target_link_libraries(nethid_module${_id}
                    pico_stdlib hardware_gpio hardware_uart hardware_i2c)
                pico_enable_stdio_usb(nethid_module${_id} 0)
                pico_enable_stdio_uart(nethid_module${_id} 1)
                pico_add_extra_outputs(nethid_module${_id})
                list(APPEND _module_targets nethid_module${_id})
            endif()
        endforeach()
        message(STATUS "NetHID: module targets: ${_module_targets}")
    endif()

    message(STATUS "NetHID: keyboard '${KEYBOARD}' keymap '${KEYMAP}'")
    message(STATUS "NetHID: keyboard features: ${_kb_on}")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_KEYBOARD=0)
    foreach(f ${KB_ALL_FEATURES})
        target_compile_definitions(nethid PRIVATE KB_FEATURE_${f}=0)
    endforeach()
    message(STATUS "NetHID: no physical keyboard (set -DKEYBOARD=<name>)")
endif()
