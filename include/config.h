#pragma once
// Local secrets live in env.h (WiFi creds, passwords). It's gitignored and is
// NOT shipped in release zips — drop your own include/env.h in before building.
// Without it, the fallbacks below let the project still compile (with placeholder
// WiFi creds that won't connect to your network).
#if defined(__has_include)
#  if __has_include("env.h")
#    include "env.h"
#  endif
#else
#  include "env.h"
#endif
// The board header first, when a board is selected. Almost everything below is
// an #ifndef default, and a default that lands BEFORE the board's own value is
// not a harmless duplicate — it wins for every use site above wherever
// keyboard.h eventually gets included, so two files can disagree about, say,
// which key is the AP-mode key. oledpad asks for AP_MODE_COL 3 while the
// default here is 5; that showed up only as a "redefined" warning.
//
// Board headers never include config.h back (they only mention it in comments),
// so there is no cycle. Every target that compiles with ENABLE_KEYBOARD=1 has
// the board directory on its include path — the firmware, the per-module
// firmwares, and every tools/kbtest target.
#if ENABLE_KEYBOARD
#include "keyboard.h"
#endif

// ============================================================
//  NetHID — User Configuration
//  Edit this file, then rebuild and flash.
// ============================================================

// ── Build identifier ────────────────────────────────────────
// Single source of truth for the build letter shown in the boot/status
// messages the device types onto the host. Bump this one symbol per build.
#define NETHID_BUILD "AO"

// ── WiFi networks ───────────────────────────────────────────
// Define one or more known networks. On boot the device scans for available
// networks and connects to the first one in THIS list that is in range
// (strongest signal wins among those present). This lets you move between
// networks without reflashing — just list them all here.
//
// Each entry: { "ssid", "password", auth_mode }
//   auth_mode: 0 = WPA2-AES only      1 = WPA/WPA2 mixed
//              2 = WPA3/WPA2 mixed     3 = WPA3 only
//              4 = open (no password)
//
// You can keep secrets out of this file by #defining them in env.h and
// referencing them here (as shown for the first entry).

typedef struct {
    const char *ssid;
    const char *password;
    int         auth_mode;
} wifi_network_t;

// Networks are defined as a single list in env.h, one entry per line:
//     #define WIFI_NETWORK_LIST   (backslash-continued, one WIFI(...) per line)
//         WIFI("Home",  "pw1", 0)
//         WIFI("Phone", "pw2", 0)
// Here we expand that list into the array by defining what WIFI(...) means.
// (This "X-macro" pattern means adding a network is one line in env.h — no
//  numbered macros, no count to maintain.)
#ifndef WIFI_NETWORK_LIST
// Fallback if env.h doesn't define any networks, so the build still compiles.
#define WIFI_NETWORK_LIST WIFI("SSID", "PASSWORD", 0)
#endif

#define WIFI(ssid, password, auth) { (ssid), (password), (auth) },
static const wifi_network_t WIFI_NETWORKS[] = {
    WIFI_NETWORK_LIST
};
#undef WIFI

#define WIFI_NETWORK_COUNT (sizeof(WIFI_NETWORKS)/sizeof(WIFI_NETWORKS[0]))

// ── Keys held at boot ────────────────────────────────────────────────────────
// Three gestures, all sampled the same way: the position must read as pressed
// across ~32 ms of scans, so a bouncing switch on a rail that has only just
// come up cannot trigger one by accident. All three are checked before USB and
// WiFi, so a firmware that hangs later is still recoverable.
//
//   BOOTMAGIC   land in the RP2350 bootloader (the BOOTSEL button, reachable)
//   AP_MODE     serve our own WiFi network for setup instead of joining one
//   QUIET_BOOT  suppress the boot diagnostics typed into the host, once
//   LOUD_BOOT   force them ON, overriding the stored setting, once
//
// Positions are (row, col) in the matrix, so what they mean physically depends
// on the board. The defaults below assume the top row: leftmost, rightmost, and
// second-from-left. A board can override any of them in its keyboard.h if its
// layout makes a different choice obvious — these are #ifndef-guarded.
//
// Pick keys far enough apart that a slipped finger cannot land on the wrong
// one; ending up in the bootloader when you wanted a quiet boot is annoying,
// the reverse is worse.
#ifndef BOOTMAGIC_ROW
#define BOOTMAGIC_ROW   0
#endif
#ifndef BOOTMAGIC_COL
#define BOOTMAGIC_COL   0
#endif

#ifndef AP_MODE_ROW
#define AP_MODE_ROW     0
#endif
#ifndef AP_MODE_COL
#define AP_MODE_COL     5
#endif

#ifndef QUIET_BOOT_ROW
#define QUIET_BOOT_ROW  0
#endif
#ifndef QUIET_BOOT_COL
#define QUIET_BOOT_COL  1
#endif

// LOUD_BOOT is the way back. Quiet boot can be turned on persistently from the
// settings page, and a device that has been told to say nothing is exactly the
// device you cannot diagnose — so holding this key forces diagnostics ON for
// one boot, whatever the stored setting or the compiled QUIET_BOOT default say.
// It also beats the quiet-boot key if you somehow hold both: between "say
// nothing" and "say something", the recoverable choice wins.
#ifndef LOUD_BOOT_ROW
#define LOUD_BOOT_ROW   0
#endif
#ifndef LOUD_BOOT_COL
#define LOUD_BOOT_COL   2
#endif

// Optional second key for bootmagic — when both are defined, BOTH must be held.
// Useful if your top-left key is one you might plausibly rest a finger on.
// #define BOOTMAGIC_ROW_2 3
// #define BOOTMAGIC_COL_2 5

// How the boot-key sample is taken. The key must read down in every scan across
// SCANS * INTERVAL milliseconds.
#ifndef BOOTMAGIC_SCANS
#define BOOTMAGIC_SCANS 8
#endif
#ifndef BOOTMAGIC_SCAN_INTERVAL_MS
#define BOOTMAGIC_SCAN_INTERVAL_MS 4
#endif

// ── Access-point setup mode ──────────────────────────────────────────────────
// Hold AP_MODE_ROW/COL while plugging the board in and NetHID serves its own
// network instead of joining one, so you can add WiFi credentials from a phone
// with no router involved. Credentials you add there are stored in flash and
// tried BEFORE the compiled list above.
//
// Deliberately not automatic. What sits behind this access point types into
// whatever computer the Pico is plugged into, so it starts when you ask it to,
// not when the router happens to be down. Set AP_MODE_AUTO_FALLBACK to 1 if you
// want the old terminal WiFi failure to become an AP instead.
//
// AP_PASSWORD has NO default. An access point with a shipped-default password
// is an open one, so ap_mode_start() refuses to run without at least 8
// characters. Put it in env.h:
//
//     #define AP_PASSWORD "something-long"
//
#ifndef AP_SSID
#define AP_SSID "NetHID-Setup"
#endif
#ifndef AP_IP
#define AP_IP "192.168.4.1"
#endif
#ifndef AP_MODE_AUTO_FALLBACK
#define AP_MODE_AUTO_FALLBACK 0
#endif

// Legacy single-network auth mode is no longer used for connection (each
// network carries its own auth_mode above), but kept defined for reference.
#define WIFI_AUTH_MODE  0

// Regulatory country code. Setting this correctly can be the difference
// between connecting and silently failing, because it controls which
// channels the radio may use. Use the two-letter code for your location:
//   "GB" UK, "US" USA, "DE" Germany, "FR" France, etc.
// Leave as "XX" for the worldwide (most permissive-but-conservative) default.
#define WIFI_COUNTRY    "XX"

// Static IP — set STATIC_IP_SET to 1 to use a fixed address instead of DHCP.
#define STATIC_IP_SET   0
#define STATIC_IP       "192.168.1.50"
#define STATIC_MASK     "255.255.255.0"
#define STATIC_GW       "192.168.1.1"
#define STATIC_DNS      "8.8.8.8"

// ── Auth ────────────────────────────────────────────────────
// Password required before any HID commands are accepted.
// Set to "" to disable authentication entirely (not recommended).
#ifndef PASSWORD
#define PASSWORD                "changeme"
#endif

// Idle seconds before the session locks and re-auth is required.
#define SESSION_TIMEOUT_S       300     // 5 minutes

// Wrong-password attempts before lockout.
#define MAX_AUTH_ATTEMPTS       5

// Lockout duration in seconds after too many wrong attempts.
#define LOCKOUT_S               30

// Shortest password /api/password will accept when changing it from the web UI.
// This is a floor on what can be SET at runtime, not on what env.h may compile
// in — an existing short password keeps working. It is also what stops "" from
// arriving through a web form and disabling auth for the whole device.
#ifndef WEB_PASSWORD_MIN_LEN
#define WEB_PASSWORD_MIN_LEN    8
#endif

// ── Users (challenge-response login) ─────────────────────────
// Define logins in env.h as a USER_LIST, one USER(name, password) per line:
/*     #define USER_LIST \
           USER("admin", "...") \
           USER("guest", "...")                                              */
// The password is the shared secret for HMAC challenge-response and is NEVER
// sent over the network: the device issues a one-time nonce and the client
// returns HMAC-SHA256(password, nonce). If USER_LIST is not defined, a single
// "admin" user is synthesised from the legacy PASSWORD so old setups keep
// working. Auth is "enabled" if any user has a non-empty password.
#ifndef USER_LIST
#define USER_LIST USER("admin", PASSWORD)
#endif

// ── Secrets (text-shortcut expansion) ────────────────────────
// Define in env.h as a SECRET_LIST, one SECRET(name, value) per line. Referenced
// inside TEXT shortcuts as ${NAME}; the device substitutes the value when typing
// and never echoes, logs, or transmits it. Names: A-Z, 0-9, _.
#ifndef SECRET_LIST
#define SECRET_LIST   /* none configured */
#endif

// Allow legacy plaintext-password auth (the password crosses the wire). When 1,
// both plaintext and HMAC challenge-response are accepted (back-compatible).
// Set to 0 to REQUIRE HMAC: plaintext JSON/HTTP auth is rejected and the binary
// socket's 0xA0 password auth is refused (use the HTTP or JSON HMAC flow).
#ifndef ALLOW_PLAINTEXT_AUTH
#define ALLOW_PLAINTEXT_AUTH    1
#endif

// ── Per-user web-UI tabs ─────────────────────────────────────
// The web UI is served per-user: the firmware sends only the tabs a user may
// see. ADMIN_USER sees every tab automatically. Grant tabs to other users in
// env.h via TAB_GRANTS, one TAB_FOR(user, id) per line ("*" = everyone).
//
// IMPORTANT: once TAB_GRANTS is non-empty, any tab NOT listed is hidden from
// every non-admin user. Adding a tab to the firmware therefore also means
// granting it, or nobody but ADMIN_USER will ever see it.
//
// Tab ids, as of this build:
//   keyboard  mouse  macros  media  irdb  learn  control  customedit
//   keymap    physical keymap editor       (KB_FEATURE_DYNAMIC_KEYMAP)
//   wifi      WiFi credentials             (ENABLE_AP_MODE)
//   settings  runtime options              (ENABLE_SETTINGS)
//
// AP setup mode ignores this list for the tabs it serves — see the note in
// src/web.cpp. Recovery must not depend on a list you need a reflash to edit.
/*     #define TAB_GRANTS \
           TAB_FOR("*",     "keyboard") \
           TAB_FOR("alice", "media")                                         */
// Built-in tab ids: keyboard, mouse, macros, media, irdb, customedit. If TAB_GRANTS
// is left empty the UI is ungated (everyone sees all tabs).
#ifndef ADMIN_USER
#define ADMIN_USER              "admin"
#endif
#ifndef TAB_GRANTS
#define TAB_GRANTS              /* none -> ungated */
#endif

// ── Ports ───────────────────────────────────────────────────
#define TCP_PORT        9000
#define SOCKET_TLS_PORT 9443        // TLS sibling of TCP_PORT (:9000 stays plain)
#define HTTP_PORT       80
#define HTTPS_PORT      443

// ── Control interfaces (compile-time) ───────────────────────
// The device offers two control paths; each can be compiled out independently.
//   ENABLE_WEB : the HTTP server on port 80 (web UI + /api/* endpoints).
//   ENABLE_TCP : the raw TCP server on port 9000 (binary + JSON protocol).
// Set to 0 to omit that server entirely (its source isn't compiled and it's
// never started). As with ENABLE_REMOTES, the build (CMakeLists.txt) also sets
// these so CMake knows which sources to compile — keep them in sync, or pass
// -DENABLE_WEB=OFF / -DENABLE_TCP=OFF at configure time.
//
// NOTE: ENABLE_REMOTES (IR/RF) is reached through the HTTP API, so it requires
// ENABLE_WEB. If you disable the web server, the IR/RF endpoints go with it.
#ifndef ENABLE_WEB
#define ENABLE_WEB      1
#endif
#ifndef ENABLE_TCP
#define ENABLE_TCP      1
#endif

// ── HTTPS / TLS  (EXPERIMENTAL — OFF by default) ─────────────
// When ENABLE_HTTPS=1, the WEB server (only) is served over TLS on HTTPS_PORT,
// terminated on the device via lwIP altcp_tls + mbedTLS, using an embedded
// server certificate (include/server_cert.h). The raw control socket (port
// 9000) stays plain TCP. Requires ENABLE_WEB. The build must match:
//     cmake .. -DENABLE_HTTPS=ON
// Generate the certificate first with one of:
//     tools/cert/make-cert-mkcert.sh nethid.example.com        # long-lived, local CA
//     tools/cert/make-cert-letsencrypt.sh nethid.example.com   # public, Cloudflare DNS-01
#ifndef ENABLE_HTTPS
#define ENABLE_HTTPS    0     // default ON. NOTE: you must still configure with
                             // -DENABLE_HTTPS=ON so mbedTLS gets linked. To build
                             // a plain-HTTP image, set this to 0 AND omit that flag.
#endif

#if ENABLE_HTTPS && !ENABLE_WEB
#error "ENABLE_HTTPS requires ENABLE_WEB"
#endif

// Serve the control socket protocol over TLS on SOCKET_TLS_PORT, in ADDITION to
// the plaintext listener on TCP_PORT (which stays for nc / shell clients). Rides
// the same mbedTLS + certificate as HTTPS, so it requires ENABLE_HTTPS.
#ifndef ENABLE_SOCKET_TLS
#define ENABLE_SOCKET_TLS  ENABLE_HTTPS
#endif
#if ENABLE_SOCKET_TLS && !ENABLE_HTTPS
#error "ENABLE_SOCKET_TLS requires ENABLE_HTTPS (shared mbedTLS / certificate)"
#endif

// TLS record buffers dominate per-connection RAM, so they are tuned per board.
// These feed include/mbedtls_config.h (MBEDTLS_SSL_IN/OUT_CONTENT_LEN). The
// RP2040 (264 KB) is tight; the RP2350 (520 KB) can afford full-size records.
#if ENABLE_HTTPS
  #if PICO_RP2040
    #define NETHID_TLS_IN_CONTENT_LEN   8192   // RP2040: trimmed to save RAM
    #define NETHID_TLS_OUT_CONTENT_LEN  4096
    #define NETHID_TLS_MAX_CONN         1      // one TLS connection at a time
  #else
    #define NETHID_TLS_IN_CONTENT_LEN   8192   // RP2350: 8K is plenty inbound for a server
    #define NETHID_TLS_OUT_CONTENT_LEN  4096   // 4K out keeps per-conn RAM low so web +
                                               // socket TLS connections coexist in heap
    #define NETHID_TLS_MAX_CONN         2
  #endif
#endif

// ── Typing ──────────────────────────────────────────────────
// Milliseconds between keystrokes when sending a string.
#define TYPE_DELAY_MS   8

// QUIET_BOOT: set to 1 to suppress ALL boot-time typing onto the host —
// both the progress/diagnostic lines and the usage synopsis. Useful once
// your setup is stable and you already know the IP. Diagnostics still go to
// UART via printf (harmless if no serial console is attached). Set to 0 to
// restore the typed boot output for troubleshooting.
#define QUIET_BOOT      0

// If 1, the Pico types a short usage synopsis (including its own IP address
// and the web UI URL) onto the host a few seconds after connecting to WiFi.
// Handy for a headless setup: open a text editor on the host, power on the
// Pico, and it tells you where to reach it.  Set to 0 to disable.
// (Ignored when QUIET_BOOT is 1 — quiet mode suppresses this too.)
#define TYPE_IP_ON_BOOT 1

// Seconds to wait after WiFi connects before typing the synopsis — gives you
// time to click into a text editor on the host.
#define TYPE_IP_DELAY_S 5

// ── Networking ──────────────────────────────────────────────
#define MAX_TCP_CLIENTS 4
#define TCP_RECV_BUF    512
#define WIFI_RETRIES    5

// ── LED ─────────────────────────────────────────────────────
#define USE_LED         1

// ── IR blaster + 433 MHz transmitter (PIO) ──────────────────
// Master switch for the IR/RF transmit feature. Set to 0 to compile it out
// entirely: remotes.cpp and the PIO programs are not built, the /api/ir and
// /api/rf endpoints are removed, and no PIO state machines are claimed. Saves
// flash and frees PIO if you don't use IR/RF.
// NOTE: the build also sets this (-DENABLE_REMOTES=…) so CMake knows whether to
// compile remotes.cpp. The #ifndef lets the CMake value win; if you flip it
// here, set ENABLE_REMOTES in CMakeLists.txt (or -DENABLE_REMOTES=OFF) to match,
// otherwise the code is gated off but the source may still try to build.
#ifndef ENABLE_REMOTES
#define ENABLE_REMOTES  1
#endif

// The IR/RF endpoints are served over the HTTP API, so remotes need the web
// server. If the web server is compiled out, force remotes off too rather than
// leaving dangling endpoint code.
#if ENABLE_REMOTES && !ENABLE_WEB
#undef  ENABLE_REMOTES
#define ENABLE_REMOTES  0
#endif

// Default GPIO pins. Override here if these clash with your wiring.
//   IR_TX_PIN : IR LED. A bare GPIO can't drive an IR LED far — drive the LED
//               through an NPN transistor (e.g. 2N2222): GPIO -> 1k -> base,
//               emitter -> GND, LED+resistor from 5V -> collector. See README.
//   RF_TX_PIN : MX-FS-03V DATA pin. Power the module's VCC from 5V for range;
//               its DATA input is fine driven by the Pico's 3V3 GPIO.
#define IR_TX_PIN       16
#define RF_TX_PIN       17
#define IR_CARRIER_HZ   38000   // most TVs; some use 36000 or 40000

// Receive ("learn") pins. Capture is optional and only active when you arm it.
//   IR_RX_PIN : a *demodulating* IR receiver (TSOP382 / VS1838B) OUT pin. It
//               outputs the already-demodulated envelope, idles HIGH and goes
//               LOW on a mark, and is 3V3-safe. VCC->3V3, GND->GND, OUT->GPIO.
//   RF_RX_PIN : 433 MHz OOK receiver DATA pin. Prefer a superheterodyne module
//               (e.g. RXB6); the cheap green ones are extremely noisy. POWER IT
//               FROM 3V3 so DATA stays 3V3-safe — at 5V the DATA line can reach
//               5V and must go through a level shifter/divider first. RF capture
//               is best-effort (noise-prone); IR capture is clean.
// DIAGNOSTIC: expose the absolute-pointer collection (Report ID 3).
// macOS appears to bind its pointer to ONE collection per HID interface; with the
// absolute collection present it services that and ignores our relative mouse
// (Report ID 2), so relative clicks/motion do nothing while absolute works.
// Build with -DENABLE_ABS_MOUSE=0 (or set 0 here) to test that theory: relative
// input should start working, at the cost of losing absolute mode.
#ifndef ENABLE_ABS_MOUSE
#define ENABLE_ABS_MOUSE 1
#endif

// How the absolute pointer presents itself to the host. The three forms differ
// only in the descriptor; the coordinates sent are identical.
//
//   2  SEPARATE INTERFACE (default). An ordinary Generic Desktop "Mouse"
//      collection with absolute 16-bit X/Y, on a HID interface of its own.
//      This is the shape every virtual machine's "absolute pointing device"
//      uses (QEMU usb-tablet, VMware, VirtualBox), and hosts drive it with a
//      NORMAL ARROW CURSOR.
//
//      The separate interface is the point. Modes 0 and 1 both put a second
//      pointer behind the same interface as the relative mouse, and which one
//      the host binds is then a guess: that ambiguity is what the earlier
//      attempts kept running into. A host binds each HID interface on its own,
//      so there is nothing left to guess.
//
//   1  DIGITIZER. A Pen collection on the Digitizers usage page. Works on
//      macOS. On WINDOWS a Pen collection is Windows Ink: the arrow cursor is
//      replaced by the pen cursor and then hidden altogether between reports.
//      That is Windows behaving correctly for a pen, not a bug to tune out —
//      the way to get an arrow back is to stop being a pen. Keep this mode for
//      a host that turns out to prefer it.
//
//   0  LEGACY: absolute X/Y as a second report ID inside the SAME mouse
//      collection as the relative pointer. Kept because it is the smallest
//      descriptor, and because it has never had a fair trial — the branch did
//      not close its mouse collection, so it could not compile, so no build of
//      it ever reached a device.
//
// In digitizer mode only, the web UI sends POSITION on report 3 and CLICKS on
// the relative mouse (report 2) with a zero movement delta, because digitizer
// button semantics vary by host. Modes 0 and 2 carry their own buttons.
//
// This default used to sit INSIDE the #ifndef ENABLE_ABS_MOUSE block above,
// which meant passing -DENABLE_ABS_MOUSE=0 skipped it and left the mode
// undefined here — it only kept working because nethid.h carries its own
// fallback. Guard each define on itself.
//
// Verify what the descriptor actually says with:
//   python3 tools/check/dump_hid_descriptor.py --all
#ifndef ABS_MOUSE_MODE
#define ABS_MOUSE_MODE 2
#endif

// The old name for mode 1. Left as a hard error rather than an alias: it was in
// the README and in the diagnostic commands in docs/SETTINGS.md, and silently
// accepting it would build a descriptor the caller did not ask for.
#ifdef ABS_MOUSE_DIGITIZER
#error "ABS_MOUSE_DIGITIZER has been replaced by ABS_MOUSE_MODE (2 = separate interface, 1 = digitizer/Pen, 0 = legacy in-mouse XY)"
#endif

#define IR_RX_PIN       21      // GP21 = physical pin 27 (receiver OUT)
#define RF_RX_PIN       19
// Power the IR receiver from a GPIO held HIGH (~3.3 V) instead of the 3V3 rail,
// so a 3-pin receiver plugs straight across [IR_RX_PIN | GND | IR_RX_PWR_PIN] =
// physical pins 27 · 28 · 29 with no jumpers. The VS1838B draws ~1 mA, well
// within a GPIO's source capability. Set to -1 to power VCC from the 3V3 rail.
#ifndef IR_RX_PWR_PIN
#define IR_RX_PWR_PIN   22      // GP22 = physical pin 29 (receiver VCC), held HIGH
#endif
