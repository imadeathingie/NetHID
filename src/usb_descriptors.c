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
    .bcdDevice          = 0x0103,   // descriptor layout changed again
                                    // descriptor changed (pan byte now declared).
                                    // macOS caches parsed HID descriptors keyed by
                                    // VID/PID/bcdDevice, so a bump forces a re-read.
    .iManufacturer      = 0x01,
    .iProduct           = 0x02,
    .iSerialNumber      = 0x03,
    .bNumConfigurations = 0x01,
};

uint8_t const *tud_descriptor_device_cb(void) {
    return (uint8_t const *)&desc_device;
}

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

    // --- Mouse (Report ID 2) ---
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
    0xC0,              //   End Collection
    0xC0,              // End Collection
#if ENABLE_ABS_MOUSE


    // --- Absolute Pointer (Report ID 3) ---
    // Standard mouse usage but with X/Y declared Absolute over a 0..32767
    // logical range, so the cursor jumps to a coordinate rather than moving
    // by a delta. Buttons and wheel mirror the relative mouse.
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x02,        // Usage (Mouse)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x03,        //   Report ID (3)
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
    // X, Y — ABSOLUTE 16-bit, logical range 0..32767
    0x05, 0x01,        //     Usage Page (Generic Desktop)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x16, 0x00, 0x00,  //     Logical Minimum (0)
    0x26, 0xFF, 0x7F,  //     Logical Maximum (32767)
    0x75, 0x10,        //     Report Size (16 bits)
    0x95, 0x02,        //     Report Count (2)
    0x81, 0x02,        //     Input (Data, Variable, Absolute)
    // Wheel — relative signed byte
    0x09, 0x38,        //     Usage (Wheel)
    0x15, 0x81,        //     Logical Minimum (-127)
    0x25, 0x7F,        //     Logical Maximum (127)
    0x75, 0x08,        //     Report Size (8 bits)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x06,        //     Input (Data, Variable, Relative)
    0xC0,              //   End Collection
    0xC0,              // End Collection
#endif  // ENABLE_ABS_MOUSE
};

uint8_t const *tud_hid_descriptor_report_cb(uint8_t instance) {
    (void)instance;
    return desc_hid_report;
}

// ── Configuration descriptor ─────────────────────────────────────────────────
// One configuration, one HID interface, one interrupt-IN endpoint.
enum {
    ITF_NUM_HID = 0,
    ITF_NUM_TOTAL,
};

#define CONFIG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_HID_DESC_LEN)
#define EPNUM_HID        0x81   // EP 1 IN

uint8_t const desc_configuration[] = {
    TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, CONFIG_TOTAL_LEN,
                          TUSB_DESC_CONFIG_ATT_REMOTE_WAKEUP, 100),

    TUD_HID_DESCRIPTOR(ITF_NUM_HID, 0, HID_ITF_PROTOCOL_NONE,
                       sizeof(desc_hid_report), EPNUM_HID,
                       CFG_TUD_HID_EP_BUFSIZE, 1 /* polling interval ms */),
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
