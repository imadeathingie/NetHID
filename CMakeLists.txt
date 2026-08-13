cmake_minimum_required(VERSION 3.13)

include($ENV{PICO_SDK_PATH}/external/pico_sdk_import.cmake)

project(nethid C CXX ASM)
set(CMAKE_C_STANDARD   11)
set(CMAKE_CXX_STANDARD 17)

if(NOT DEFINED PICO_BOARD)
    set(PICO_BOARD pico2_w)
endif()

pico_sdk_init()

# ── Feature toggle: IR/RF remotes ─────────────────────────────────────────────
# Mirror of ENABLE_REMOTES in include/config.h. Keep the two in sync — this one
# controls what gets COMPILED/LINKED; the config.h one is passed to the
# preprocessor below so the firmware code agrees. Override from the command line
# with:  cmake .. -DENABLE_REMOTES=OFF
option(ENABLE_REMOTES "Build the IR blaster + 433 MHz transmit feature" ON)
option(ENABLE_WEB     "Build the HTTP web server / API (port 80)"        ON)
option(ENABLE_TCP     "Build the raw TCP control server (port 9000)"     ON)
option(ENABLE_HTTPS   "Serve the web UI over TLS (HTTPS, port 443)"      OFF)

# The absolute pointer. Both of these are diagnostic switches for the open
# absolute-mouse fault (see docs/SETTINGS.md), and they are options rather than
# edit-config.h-by-hand because a branch you cannot build from the command line
# is a branch nobody builds — which is how the ABS_MOUSE_DIGITIZER=0 path came
# to contain a descriptor that did not close its mouse collection.
option(ENABLE_ABS_MOUSE "Expose the absolute pointer (HID report 3)"      ON)
set(ABS_MOUSE_MODE 2 CACHE STRING
    "Absolute pointer form: 2 = own HID interface (default), 1 = digitizer/Pen, 0 = legacy in-mouse XY")

# The old name for the mode. Not a fatal error and not silently honoured: a
# build directory configured before the rename still has it in its cache, so
# erroring would break `cmake --build` on an existing tree for something nobody
# typed. Warn, drop it, and carry on with ABS_MOUSE_MODE — deliberately WITHOUT
# mapping the old value, because the old default (digitizer) is precisely the
# form this rename exists to stop being the default.
if(DEFINED ABS_MOUSE_DIGITIZER)
    message(WARNING
        "ABS_MOUSE_DIGITIZER is gone and has been ignored. Use "
        "-DABS_MOUSE_MODE=1 for the old digitizer/Pen form, 0 for the legacy "
        "in-mouse XY form, or leave it unset for the default (2: absolute "
        "mouse on its own HID interface). Now using ABS_MOUSE_MODE=${ABS_MOUSE_MODE}.")
    unset(ABS_MOUSE_DIGITIZER CACHE)
    unset(ABS_MOUSE_DIGITIZER)
endif()
if(NOT ABS_MOUSE_MODE MATCHES "^[012]$")
    message(FATAL_ERROR "ABS_MOUSE_MODE must be 0, 1 or 2 (got '${ABS_MOUSE_MODE}')")
endif()

# Remotes are served over the HTTP API, so they require the web server.
if(ENABLE_REMOTES AND NOT ENABLE_WEB)
    message(WARNING "ENABLE_REMOTES needs ENABLE_WEB; disabling remotes.")
    set(ENABLE_REMOTES OFF)
endif()
# TLS wraps the web server, so it requires it too.
if(ENABLE_HTTPS AND NOT ENABLE_WEB)
    message(FATAL_ERROR "ENABLE_HTTPS needs ENABLE_WEB.")
endif()

add_executable(nethid
    src/main.cpp
    src/hid.cpp
    src/hid_layout.cpp
    src/auth.cpp
    src/auth_store.cpp
    src/hmac_sha256.cpp
    src/secrets.cpp
    src/tabs.cpp
    src/usb_descriptors.c
)

if(ENABLE_WEB)
    target_sources(nethid PRIVATE src/web.cpp)
    target_compile_definitions(nethid PRIVATE ENABLE_WEB=1)
    message(STATUS "NetHID: web server ENABLED")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_WEB=0)
    message(STATUS "NetHID: web server disabled")
endif()

if(ENABLE_ABS_MOUSE)
    set(_abs_form "own HID interface")
    if(ABS_MOUSE_MODE EQUAL 1)
        set(_abs_form "digitizer/Pen - Windows will show a pen cursor, not an arrow")
    elseif(ABS_MOUSE_MODE EQUAL 0)
        set(_abs_form "legacy, shares the mouse collection")
    endif()
    target_compile_definitions(nethid PRIVATE
        ENABLE_ABS_MOUSE=1 ABS_MOUSE_MODE=${ABS_MOUSE_MODE})
    message(STATUS "NetHID: absolute pointer ENABLED (mode ${ABS_MOUSE_MODE}: ${_abs_form})")
else()
    # Still define the mode: tusb_config.h sizes CFG_TUD_HID from it, and an
    # undefined macro there would quietly size the stack for one interface.
    target_compile_definitions(nethid PRIVATE ENABLE_ABS_MOUSE=0 ABS_MOUSE_MODE=${ABS_MOUSE_MODE})
    message(STATUS "NetHID: absolute pointer disabled")
endif()

if(ENABLE_TCP)
    target_sources(nethid PRIVATE src/server.cpp)
    target_compile_definitions(nethid PRIVATE ENABLE_TCP=1)
    message(STATUS "NetHID: TCP server ENABLED")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_TCP=0)
    message(STATUS "NetHID: TCP server disabled")
endif()

if(ENABLE_REMOTES)
    target_sources(nethid PRIVATE src/remotes.cpp)
    # Generate C headers from the PIO programs (IR blaster + 433 MHz OOK).
    pico_generate_pio_header(nethid ${CMAKE_CURRENT_SOURCE_DIR}/src/rf_ook.pio)
    pico_generate_pio_header(nethid ${CMAKE_CURRENT_SOURCE_DIR}/src/ir_tx.pio)
    # Define for the preprocessor so config.h / main.cpp / web.cpp agree.
    target_compile_definitions(nethid PRIVATE ENABLE_REMOTES=1)
    message(STATUS "NetHID: IR/RF remotes ENABLED")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_REMOTES=0)
    message(STATUS "NetHID: IR/RF remotes disabled")
endif()

target_include_directories(nethid PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/include/lwip
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# ── Build id ──────────────────────────────────────────────────────────────────
# A value that differs for every build, used by the web-password store to tell
# "written by the firmware I am" from "written by the one before it". A stored
# password whose build id does not match is ignored, which is what makes
# reflashing the recovery route for a forgotten password.
#
# A custom target with no OUTPUT re-runs every build, which is the point: a
# configure-time value would stay put across rebuilds and a reflash would then
# inherit the old password — the one thing this must not do.
add_custom_target(nethid_build_id ALL
    COMMAND ${CMAKE_COMMAND}
            -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/nethid_build_id.h
            -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake
    COMMENT "Generating build id"
    VERBATIM)
add_dependencies(nethid nethid_build_id)

# Generate it once at configure time too, so the first compile has a header to
# include before the custom target has ever run.
execute_process(COMMAND ${CMAKE_COMMAND}
    -DOUT=${CMAKE_CURRENT_BINARY_DIR}/generated/nethid_build_id.h
    -P ${CMAKE_CURRENT_SOURCE_DIR}/cmake/build_id.cmake)

# ── Web password store ────────────────────────────────────────────────────────
# Changing the login password from the web UI. Follows ENABLE_WEB rather than
# being its own option: without the web UI there is nothing to change it from.
#
# src/auth_store.cpp is compiled either way and guards its own body, because
# main.cpp calls auth_store_init() and auth_store_commit_poll() unconditionally
# and would otherwise need the same #if in a second place. The stub branch keeps
# the "who decides whether this file has contents" answer in one file.
if(ENABLE_WEB)
    target_link_libraries(nethid hardware_flash hardware_sync pico_multicore)
    target_compile_definitions(nethid PRIVATE ENABLE_PASSWORD_STORE=1)
    message(STATUS "NetHID: web password can be changed at runtime")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_PASSWORD_STORE=0)
endif()

# Runtime settings: config.h options that can be changed from the web UI and
# persisted, without a reflash. See include/settings.h for what is deliberately
# NOT included.
option(ENABLE_SETTINGS "Build the runtime settings store" ON)
if(ENABLE_SETTINGS)
    target_sources(nethid PRIVATE src/settings.cpp)
    target_link_libraries(nethid hardware_flash hardware_sync pico_multicore)
    target_compile_definitions(nethid PRIVATE ENABLE_SETTINGS=1)
else()
    target_compile_definitions(nethid PRIVATE ENABLE_SETTINGS=0)
endif()

# Access-point fallback: NetHID serves its own network so it can be configured
# without a router. Needs a DHCP server and a DNS server, neither of which lwIP
# ships — see src/vendor/README.md.
option(ENABLE_AP_MODE "Build AP setup mode" ON)
if(ENABLE_AP_MODE)
    target_sources(nethid PRIVATE
        src/ap_mode.cpp
        src/wifi_store.cpp
        src/vendor/dhcpserver.c
        src/vendor/dnsserver.c
    )
    target_include_directories(nethid PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/src)
    target_link_libraries(nethid hardware_flash hardware_sync hardware_watchdog pico_multicore)
    target_compile_definitions(nethid PRIVATE ENABLE_AP_MODE=1)
else()
    target_compile_definitions(nethid PRIVATE ENABLE_AP_MODE=0)
endif()

# Physical keyboard (matrix scanning, keymaps, quantum features).
include(${CMAKE_CURRENT_SOURCE_DIR}/cmake/keyboard.cmake)

target_link_libraries(nethid
    pico_stdlib
    pico_rand
    pico_multicore
    pico_cyw43_arch_lwip_threadsafe_background
    tinyusb_device
    tinyusb_board
    hardware_sync
)
if(ENABLE_REMOTES)
    target_link_libraries(nethid hardware_pio)
endif()

# ── HTTPS / TLS (experimental) ───────────────────────────────────────────────
# Links mbedTLS + the lwIP-mbedTLS glue and points mbedTLS at our config. NOTE:
# you must ALSO set `#define ENABLE_HTTPS 1` in include/config.h — lwIP is compiled
# separately and reads the flag through lwipopts.h -> config.h, not via -D here.
# VERIFY ON HARDWARE: the two library names below (pico_mbedtls, pico_lwip_mbedtls)
# are the SDK's TLS libs; if CMake can't find them, check the names your SDK 2.2.0
# uses in pico-examples/pico_w/wifi/tls_*.
if(ENABLE_HTTPS)
    if(NOT EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/include/server_cert.h)
        message(FATAL_ERROR
            "ENABLE_HTTPS=ON but include/server_cert.h is missing. Generate it first:\n"
            "  ./tools/make-cert-mkcert.sh nethid.example.com        (long-lived, local CA)\n"
            "  ./tools/make-cert-letsencrypt.sh nethid.example.com   (public, Cloudflare DNS-01)")
    endif()
    # NOTE: do not set MBEDTLS_CONFIG_FILE here. The Pico SDK's pico_mbedtls
    # already sets it (to pico_mbedtls_config.h, which #includes our
    # include/mbedtls_config.h by name). Setting it again collides and produces
    # a flood of "'MBEDTLS_CONFIG_FILE' redefined" warnings on every TU.
    # TLS needs a much larger C heap (mbedTLS allocates the SSL context + record
    # buffers there). Size by board: the RP2350 (520 KB SRAM) gets a big heap;
    # the RP2040 (264 KB) a smaller one.
    if(PICO_BOARD MATCHES "pico2")
        set(NETHID_TLS_HEAP 196608)   # 192 KB
    else()
        set(NETHID_TLS_HEAP 98304)    # 96 KB
    endif()
    target_compile_definitions(nethid PRIVATE
        ENABLE_HTTPS=1
        PICO_HEAP_SIZE=${NETHID_TLS_HEAP}
        PICO_STACK_SIZE=0x1000)        # 4 KB: the max a scratch-resident stack can be on
                                       # RP2350 (SCRATCH_X/Y are 4 KB). Double the 2 KB default,
                                       # to give the mbedTLS handshake headroom.
    target_link_libraries(nethid pico_mbedtls pico_lwip_mbedtls pico_rand)
    # mbedTLS has no bare-metal mbedtls_ms_time(); we provide it (see config).
    target_sources(nethid PRIVATE src/mbedtls_pico_time.c)
    message(STATUS "NetHID: HTTPS/TLS ENABLED (heap=${NETHID_TLS_HEAP}; also set ENABLE_HTTPS=1 in include/config.h!)")
else()
    target_compile_definitions(nethid PRIVATE ENABLE_HTTPS=0)
    message(STATUS "NetHID: HTTPS/TLS disabled (plain HTTP)")
endif()

pico_enable_stdio_usb(nethid 0)
pico_enable_stdio_uart(nethid 1)

pico_add_extra_outputs(nethid)

target_compile_options(nethid PRIVATE -O2 -Wall -Wextra)
