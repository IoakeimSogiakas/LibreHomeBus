/*
 * Modbus RTU slave over RS-485 for the CH32V003, with automatic address
 * discovery. See config.h for the build flags, discovery.c for the
 * discovery frames.
 */

#include "node.h"

int main(void)
{
    SystemInit();
    debug_init();
    uart_init();
    discovery_init();
    flavor_init();
    debugln("flavor %d, id %d%s, uid %08X%08X", FLAVOR, slave_id,
            DEFAULT_ID ? " (fixed)" : "",
            (unsigned)(my_uid >> 32), (unsigned)my_uid);

    while (1) {
        flavor_poll();

        if (frame_poll()) {
            if (rx_overflow) {
                debugln("overflow, dropped");
            } else if (rx_len < 4) {
                debugln("too short (%d bytes)", rx_len);
            } else if (!crc_good()) {
                debugln("bad CRC");
            } else if (rx_buf[0] == ID_BROADCAST) {
                handle_broadcast();         /* no logging here, debugln is slow and causes errors maybe issue with DEBUGPRINTF */
            } else if (rx_buf[0] != slave_id) {
                debugln("not for me (id %d)", rx_buf[0]);
            } else {
                handle_frame();             /* reply first, log after */
                debugln("handled func %02X", rx_buf[1]);
            }

            rx_len = 0;
            rx_overflow = 0;
        }
    }
}