// mbedtls_pico_time.c — provide mbedtls_ms_time() on bare-metal RP2040/RP2350.
//
// mbedTLS ships a built-in mbedtls_ms_time() only for POSIX/Windows; on the Pico
// neither matches, so mbedtls_config.h defines MBEDTLS_PLATFORM_MS_TIME_ALT and
// we supply it here from the Pico's monotonic microsecond clock. mbedTLS only
// needs this to be monotonically increasing (TLS 1.3 ticket lifetimes / timers),
// not wall-clock accurate. Compiled into the build only when ENABLE_HTTPS.
#include "mbedtls/build_info.h"
#include "mbedtls/platform_time.h"
#include "pico/time.h"

mbedtls_ms_time_t mbedtls_ms_time(void) {
    return (mbedtls_ms_time_t)(time_us_64() / 1000u);
}
