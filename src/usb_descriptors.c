/*
 * usb_descriptors.c
 * TinyUSB USB device descriptors for NetHID.
 *
 * Presents as a composite HID device with two report IDs:
 *   ID 1 – Keyboard (8-byte boot-compatible report)
 *   ID 2 – Mouse    (4-byte relative report)
 *
 * VID/PID chosen from the Raspberry Pi / TinyUSB demo range.
 * Change them if you need stable OS-driver assignment.
 */

#include "tusb.h"

// ── Host-OS detection (see tud_descriptor_string_cb) ─────────────────────────
// Written by the USB stack (core 1) during enumeration, read by the web server
// (core 0). A single byte status flag; a torn read isn't possible for a byte,
// and worst case the web UI reads it one poll early/late, which is harmless.
#include "nethid.h"   // for host_os_t / usb_host_os()
volatile uint8_t g_host_os = HOST_OS_UNKNOWN;

uint8_t usb_host_os(void) {
    return g_host_os;
}

// ── Device descriptor ────────────────────────────────────────────────────────
tusb_desc_device_t const desc_device = {
    .bLength            = sizeof(tusb_desc_device_t),
    .bDescriptorType    = TUSB_DESC_DEVICE,
    .bcdUSB             = 0x0200,
    .bDeviceClass       = 0x00,             // Each interface defines its own class
    .bDeviceSubClass    = 0x00,
    .bDeviceProtocol    = 0x00,
    .bMaxPacketSize0    = CFG_TUD_ENDPOINT0_SIZE,
    .idVendor           = 0x2E8A,           // Raspberry Pi
    .idProduct          = 0x000A,           // Demo HID
    .bcdDevice          = 0x0107,   // 0x0105 declared a Physical range that
                                    // leaked onto the wheel; the items are gone
                                    // and a host holding the old parse would
                                    // still be rescaling. Below:
                                    // The absolute pointer moved to a HID
                                    // interface of its own, so the interface
                                    // COUNT changed, not just a report — a host
                                    // that reuses a cached parse would look for
                                    // an endpoint that no longer means what it
                                    // did. macOS and Windows both cache parsed
                                    // descriptors keyed by VID/PID/bcdDevice, so
                                    // this must be bumped whenever the shape
                                    // changes. 0x0104 was the previous shape
                                    // (Pen digitizer on one interface).
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

// The absolute pointer's report body: buttons, absolute 16-bit X/Y, relative
// wheel. Defined once because modes 0 and 2 are the SAME report in a different
// place — one inside the relative mouse's collection, one on its own interface.
// Written out twice they would drift, and a descriptor that drifts is a device
// whose two build modes disagree about the shape of a report they both send.
#define ABS_POINTER_BODY                                                       \
    0x85, 0x03,        /*   Report ID (3)                                   */ \
    0x09, 0x01,        /*   Usage (Pointer)                                 */ \
    0xA1, 0x00,        /*   Collection (Physical)                           */ \
    /* Buttons: 3 buttons + 5 padding bits */                                  \
    0x05, 0x09,        /*     Usage Page (Buttons)                          */ \
    0x19, 0x01,        /*     Usage Minimum (Button 1)                      */ \
    0x29, 0x03,        /*     Usage Maximum (Button 3)                      */ \
    0x15, 0x00,        /*     Logical Minimum (0)                           */ \
    0x25, 0x01,        /*     Logical Maximum (1)                           */ \
    0x95, 0x03,        /*     Report Count (3)                              */ \
    0x75, 0x01,        /*     Report Size (1 bit)                           */ \
    0x81, 0x02,        /*     Input (Data, Variable, Absolute)              */ \
    0x95, 0x01,        /*     Report Count (1) - padding                    */ \
    0x75, 0x05,        /*     Report Size (5 bits)                          */ \
    0x81, 0x01,        /*     Input (Constant)                              */ \
    /* X, Y - ABSOLUTE 16-bit, logical range 0..32767.                        \
     *                                                                        \
     * NO Physical Minimum/Maximum here, deliberately. They are GLOBAL items:  \
     * once set they stay in effect for every main item that follows until     \
     * changed. Declaring Physical 0..32767 for X/Y therefore also applied it  \
     * to the Wheel below, whose logical range is -127..127 — so a host that   \
     * converts to physical units read a wheel of 0 as the midpoint of 0..32767\
     * and scrolled on every single report. Windows does that conversion;      \
     * macOS did not, which is why it only showed on one of them.              \
     *                                                                        \
     * Physical min/max both absent (or both 0) means "physical units are the  \
     * logical units", which is what is wanted and what QEMU's usb-tablet — the\
     * descriptor this mode is modelled on — does. */                          \
    0x05, 0x01,        /*     Usage Page (Generic Desktop)                  */ \
    0x09, 0x30,        /*     Usage (X)                                     */ \
    0x09, 0x31,        /*     Usage (Y)                                     */ \
    0x16, 0x00, 0x00,  /*     Logical Minimum (0)                           */ \
    0x26, 0xFF, 0x7F,  /*     Logical Maximum (32767)                       */ \
    0x75, 0x10,        /*     Report Size (16 bits)                         */ \
    0x95, 0x02,        /*     Report Count (2)                              */ \
    0x81, 0x02,        /*     Input (Data, Variable, Absolute)              */ \
    /* Wheel - relative signed byte */                                         \
    0x09, 0x38,        /*     Usage (Wheel)                                 */ \
    0x15, 0x81,        /*     Logical Minimum (-127)                        */ \
    0x25, 0x7F,        /*     Logical Maximum (127)                         */ \
    0x75, 0x08,        /*     Report Size (8 bits)                          */ \
    0x95, 0x01,        /*     Report Count (1)                              */ \
    0x81, 0x06,        /*     Input (Data, Variable, Relative)              */ \
    0xC0,              /*   End Collection   (physical - absolute pointer)  */

// ── HID report descriptor ────────────────────────────────────────────────────
// Keyboard (Report ID 1) + Mouse (Report ID 2) in a single interface.
uint8_t const desc_hid_report[] = {
    // --- Keyboard (Report ID 1) ---
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x06,        // Usage (Keyboard)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    // Modifier keys byte
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0xE0,        //   Usage Minimum (Left Ctrl)
    0x29, 0xE7,        //   Usage Maximum (Right GUI)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1 bit)
    0x95, 0x08,        //   Report Count (8)
    0x81, 0x02,        //   Input (Data, Variable, Absolute)
    // Reserved byte
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x08,        //   Report Size (8 bits)
    0x81, 0x01,        //   Input (Constant)
    // Key array — up to 6 simultaneous keys
    0x95, 0x06,        //   Report Count (6)
    0x75, 0x08,        //   Report Size (8 bits)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0xFF,        //   Logical Maximum (255)
    0x05, 0x07,        //   Usage Page (Key Codes)
    0x19, 0x00,        //   Usage Minimum (0)
    0x29, 0xFF,        //   Usage Maximum (255)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection

    // --- Pointing devices: ONE Mouse collection, two report IDs ---
    //
    // Relative (ID 2) and absolute (ID 3) live inside the SAME Application
    // collection. They used to be two separate collections that both declared
    // Usage (Mouse), and hosts commonly bind only the first one they see —
    // which is exactly why the relative mouse worked and the absolute one did
    // not, and why the two appeared to fight over button presses.
    //
    // A single Application collection may contain several report IDs, so this
    // costs nothing and removes the ambiguity entirely: there is one mouse, and
    // it can be told to move by a delta or to jump to a coordinate.
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)

    0x85, 0x02,        //   Report ID (2)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    // Buttons: 3 buttons + 5 padding bits
    0x05, 0x09,        //     Usage Page (Buttons)
    0x19, 0x01,        //     Usage Minimum (Button 1)
    0x29, 0x03,        //     Usage Maximum (Button 3)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x03,        //     Report Count (3)
    0x75, 0x01,        //     Report Size (1 bit)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    0x95, 0x01,        //     Report Count (1) — padding
    0x75, 0x05,        //     Report Size (5 bits)
    0x81, 0x01,        //     Input (Constant)
    // X, Y, Wheel — relative signed bytes
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8 bits)
    0x95, 0x03,        //     Report Count (3)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    // 5th byte: TinyUSB's hid_mouse_report_t has a trailing `pan` byte, so the
    // report is 5 bytes and the descriptor must account for all 5. We declare it
    // as CONSTANT padding rather than a functional Consumer-page AC Pan usage:
    // some macOS versions reject/ignore a relative Mouse collection that mixes in
    // a Consumer AC Pan field and then drop the whole report (including the
    // button bits) — which is exactly the "clicks do nothing on the Mac, but the
    // absolute pointer works" symptom. We always send pan=0 anyway.
    0x75, 0x08,        //     Report Size (8 bits)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x01,        //     Input (Constant)  — padding, keeps report at 5 bytes
    0xC0,              //   End Collection   (physical — relative pointer)

#if !ENABLE_ABS_MOUSE || ABS_MOUSE_MODE != 0
    // Close the mouse Application collection here, because nothing further
    // belongs inside it: there is no absolute pointer at all, or it is a
    // digitizer (its own Application collection below), or it is on an entirely
    // separate interface. Only the legacy mode puts a Physical collection for
    // the absolute pointer INSIDE this one, so only there does the 0xC0 move.
    0xC0,              // End Collection   (mouse Application)
#endif

#if ENABLE_ABS_MOUSE
#if ABS_MOUSE_MODE == 1
    //   --- Absolute pointer as a DIGITIZER (Report ID 3) ---
    //
    // A second Generic Desktop "Mouse" collection with absolute X/Y is accepted
    // by hosts and then, on Windows at least, ignored: the mouse class driver
    // binds one set of X/Y per device and treats it as relative. Reports go out,
    // nothing moves, and there is no error anywhere to find.
    //
    // A digitizer is a different device class with absolute coordinates as its
    // whole point, so the host has no reason to reinterpret them. TIP SWITCH and
    // IN RANGE are not optional decoration — a host will not track a pointer it
    // does not believe is near the surface, so IN RANGE must be set on every
    // report that carries a position.
    //
    // Buttons deliberately do NOT live here. Digitizer button semantics vary by
    // host (barrel switch is right-click on some, nothing on others), and the
    // relative mouse on report 2 already does buttons correctly everywhere. So
    // position comes from this report and clicks from that one, sent with a
    // zero movement delta — each doing the thing it is reliably good at.
    0x05, 0x0D,        //   Usage Page (Digitizers)
    0x09, 0x02,        //   Usage (Pen)
    0xA1, 0x01,        //   Collection (Application)
    0x85, 0x03,        //     Report ID (3)
    0x09, 0x20,        //     Usage (Stylus)
    0xA1, 0x00,        //     Collection (Physical)
    0x09, 0x42,        //       Usage (Tip Switch)
    0x09, 0x32,        //       Usage (In Range)
    0x15, 0x00,        //       Logical Minimum (0)
    0x25, 0x01,        //       Logical Maximum (1)
    0x75, 0x01,        //       Report Size (1 bit)
    0x95, 0x02,        //       Report Count (2)
    0x81, 0x02,        //       Input (Data, Variable, Absolute)
    0x95, 0x06,        //       Report Count (6) - padding
    0x81, 0x03,        //       Input (Constant)
    0x05, 0x01,        //       Usage Page (Generic Desktop)
    0x09, 0x30,        //       Usage (X)
    0x09, 0x31,        //       Usage (Y)
    0x16, 0x00, 0x00,  //       Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //       Logical Maximum (32767)
    // No Physical Minimum/Maximum. They were here, equal to the logical range
    // and so contributing nothing — but they are GLOBAL items, and the Consumer
    // Control collection that follows this one in the same descriptor inherited
    // them against its own 0..0x3FF logical range. See ABS_POINTER_BODY for the
    // damage the same leak did to the wheel.
    0x75, 0x10,        //       Report Size (16 bits)
    0x95, 0x02,        //       Report Count (2)
    0x81, 0x02,        //       Input (Data, Variable, Absolute)
    0xC0,              //     End Collection
    0xC0,              //   End Collection

#elif ABS_MOUSE_MODE == 0
    //   --- Absolute pointer sharing the mouse collection (Report ID 3) ---
    ABS_POINTER_BODY
    0xC0,              // End Collection   (mouse Application)

#endif  // ABS_MOUSE_MODE
#endif  // ENABLE_ABS_MOUSE


    // --- Consumer Control (Report ID 4) ---
    //
    // Volume, transport, brightness and the browser keys. These are NOT keyboard
    // usages and cannot be sent on report 1 — that is why KC_MUTE and friends
    // did not exist until now.
    //
    // One 16-bit usage per report rather than a bitmap of named keys: the
    // consumer page is enormous and mostly momentary, so "here is the usage
    // currently pressed, or 0" is both smaller and simpler than a fixed set of
    // bits that would need extending every time a new key is wanted.
    0x05, 0x0C,        // Usage Page (Consumer)
    0x09, 0x01,        // Usage (Consumer Control)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x04,        //   Report ID (4)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (0x3FF)
    0x19, 0x00,        //   Usage Minimum (0)
    0x2A, 0xFF, 0x03,  //   Usage Maximum (0x3FF)
    0x75, 0x10,        //   Report Size (16 bits)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x00,        //   Input (Data, Array)
    0xC0,              // End Collection


};

#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
// ── Absolute pointer, on its own HID interface ───────────────────────────────
//
// An ordinary Generic Desktop "Mouse" whose X/Y happen to be absolute — the
// same thing every VM presents as its "absolute pointing device", and hosts
// drive it with a normal arrow cursor.
//
// The reason it is a second INTERFACE rather than a second collection is the
// whole point of this mode. A host decides for itself which of several pointer
// collections behind one interface to bind, and that guess is what the earlier
// attempts kept losing: two Mouse collections meant the relative one won and
// absolute reports went nowhere; folding both into one collection meant the
// host applied one interpretation of X/Y to both report IDs. Interfaces are
// bound independently, so neither question arises.
uint8_t const desc_hid_report_abs[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    ABS_POINTER_BODY
    0xC0,              // End Collection   (mouse Application)
};
#endif

// ── Interface numbering ──────────────────────────────────────────────────────
// One HID interface, or two when the absolute pointer has one of its own. This
// is also the TinyUSB *instance* number, which is what tud_hid_n_report() takes.
enum {
    ITF_NUM_HID = 0,
#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
    ITF_NUM_HID_ABS,
#endif
    ITF_NUM_TOTAL,
};

// The descriptor declares the interfaces; CFG_TUD_HID sizes the state TinyUSB
// keeps per interface. If they disagree the device still enumerates and then
// silently drops every report on the interface TinyUSB does not know it has —
// no error at build time, none at run time, and on the host it looks exactly
// like "the absolute mouse doesn't work". Both derive from ABS_MOUSE_MODE, so
// this can only fire if someone edits one of them by hand; that is precisely
// when it is worth having.
TU_VERIFY_STATIC(ITF_NUM_TOTAL == CFG_TUD_HID,
                 "CFG_TUD_HID in tusb_config.h must match the number of HID "
                 "interfaces this descriptor declares");

// An out-of-range mode selects NONE of the branches above: the mouse collection
// closes, no absolute pointer is declared, and hid.cpp goes on sending report 3
// to a report ID the host was never told about. CMake range-checks the option,
// but a hand-edited config.h does not go through CMake.
TU_VERIFY_STATIC(ABS_MOUSE_MODE == 0 || ABS_MOUSE_MODE == 1 || ABS_MOUSE_MODE == 2,
                 "ABS_MOUSE_MODE must be 0 (legacy in-mouse XY), 1 (digitizer/Pen) "
                 "or 2 (own HID interface)");

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
    if (instance == ITF_NUM_HID_ABS) return desc_hid_report_abs;
#endif
    (void)instance;
    return desc_hid_report;
}

// ── Configuration descriptor ─────────────────────────────────────────────────

#define EPNUM_HID        0x81   // EP 1 IN
#define EPNUM_HID_ABS    0x82   // EP 2 IN

#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + 2 * TUD_HID_DESC_LEN)
#else
#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#endif

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 1 /* polling interval ms */),

#if ENABLE_ABS_MOUSE && ABS_MOUSE_MODE == 2
    // HID_ITF_PROTOCOL_NONE, not _MOUSE: the boot-protocol mouse report is
    // relative by definition, so claiming it would invite a host to fall back
    // to a shape this interface does not send.
    TUD_HID_DESCRIPTOR(ITF_NUM_HID_ABS, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report_abs), EPNUM_HID_ABS,
                       CFG_TUD_HID_EP_BUFSIZE, 1 /* polling interval ms */),
#endif
};

uint8_t const *tud_descriptor_configuration_cb(uint8_t index) {
    (void)index;
    return desc_configuration;
}

// ── String descriptors ───────────────────────────────────────────────────────
static char const *string_desc_arr[] = {
    (const char[]){0x09, 0x04},   // 0: supported language = English (0x0409)
    "NetHID",                      // 1: Manufacturer
    "NetHID Keyboard+Mouse",       // 2: Product
    "000001",                      // 3: Serial number
};

static uint16_t _desc_str[32];

uint16_t const *tud_descriptor_string_cb(uint8_t index, uint16_t langid) {
    (void)langid;
    uint8_t chr_count;

    // ── Host-OS detection ─────────────────────────────────────────────────────
    // Windows probes for the "Microsoft OS String Descriptor" at string index
    // 0xEE during enumeration; macOS and Linux do not. Seeing this request is a
    // strong, well-documented signal that the USB host is Windows. We just note
    // it and fall through to the normal (NULL/STALL) handling, which is the
    // correct response for a device that doesn't implement MS OS descriptors.
    if (index == 0xEE) {
        g_host_os = HOST_OS_WINDOWS;
    }

    if (index == 0) {
        memcpy(&_desc_str[1], string_desc_arr[0], 2);
        chr_count = 1;
    } else {
        if (index >= sizeof(string_desc_arr) / sizeof(string_desc_arr[0]))
            return NULL;
        const char *str = string_desc_arr[index];
        chr_count = (uint8_t)strlen(str);
        if (chr_count > 31) chr_count = 31;
        for (uint8_t i = 0; i < chr_count; i++)
            _desc_str[1 + i] = str[i];
    }

    // First word: total byte length + string descriptor type
    _desc_str[0] = (uint16_t)((TUSB_DESC_STRING << 8) | (2 * chr_count + 2));
    return _desc_str;
}
