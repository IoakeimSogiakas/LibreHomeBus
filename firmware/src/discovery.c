/*
 * Automatic address discovery and the address kept in flash.
 *
 * Discovery (broadcast address 0, custom function codes, no Modbus replies):
 *   00 41 <uid:8> crc          unassigned chips with UID <= this answer 0x55
 *   00 42 <uid:8> <addr> crc   chip with this UID takes <addr> and saves it
 *   00 43 <uid:8> crc          chip with this UID forgets its address
 */

#include "node.h"


/* ---- protocol ---------------------------------------------------------- */

/* 0x41-0x48 is reserved for user-defined functions in the Modbus spec,
 * so these can't collide with anything standard. FN_COMPARE is in node.h. */
#define FN_ASSIGN         0x42
#define FN_FORGET         0x43

/* Master only checks that something arrived, the value doesn't matter. */
#define REPLY_BYTE        0x55

/* Exact frame lengths: address + function + payload + 2 CRC bytes.
 * LEN_COMPARE is in node.h. */
#define LEN_ASSIGN        13
#define LEN_FORGET        12

#define ID_MAX            247


/* ---- chip UID ---------------------------------------------------------- */

uint64_t my_uid;

/* Big-endian: same order the boot print shows and the master sends it. */
static uint64_t be64(const uint8_t *p)
{
    uint64_t v = 0;
    for (int i = 0; i < 8; i++) v = (v << 8) | p[i];
    return v;
}


/* ---- address storage in flash ------------------------------------------ */

#if DEFAULT_ID == 0
/* Flash erases and writes in 64-byte pages, so the address gets the last
 * page to itself. Record layout (32-bit words):
 *   0: magic    1: address    2: address inverted (corruption check)
 * Reflashing the firmware doesn't touch this page. */
#define ID_PAGE_ADDR  0x08003FC0UL
#define ID_MAGIC      0x4D4F4442UL

#define FL_BSY      (1u << 0)
#define FL_STRT     (1u << 6)
#define FL_LOCK     (1u << 7)
#define FL_FLOCK    (1u << 15)
#define FL_FTPG     (1u << 16)     /* fast page program */
#define FL_FTER     (1u << 17)     /* fast page erase */
#define FL_BUFLOAD  (1u << 18)
#define FL_BUFRST   (1u << 19)

static void flash_wait(void)
{
    while (FLASH->STATR & FL_BSY);
}

/* id_save(0) clears the stored address. Blocks for a few ms and the chip
 * can't receive in that time. */
static void id_save(uint8_t id)
{
    uint32_t page[16];
    for (int i = 0; i < 16; i++) page[i] = 0xFFFFFFFF;
    page[0] = ID_MAGIC;
    page[1] = id;
    page[2] = ~(uint32_t)id;

    /* unlock sequence from the reference manual */
    FLASH->KEYR     = 0x45670123;
    FLASH->KEYR     = 0xCDEF89AB;
    FLASH->MODEKEYR = 0x45670123;
    FLASH->MODEKEYR = 0xCDEF89AB;

    FLASH->CTLR = FL_FTER;
    FLASH->ADDR = ID_PAGE_ADDR;
    FLASH->CTLR |= FL_STRT;
    flash_wait();
    FLASH->CTLR = 0;

    FLASH->CTLR = FL_FTPG;
    FLASH->CTLR |= FL_BUFRST;
    flash_wait();
    for (int i = 0; i < 16; i++) {
        *(volatile uint32_t *)(ID_PAGE_ADDR + 4 * i) = page[i];
        FLASH->CTLR |= FL_BUFLOAD;
        flash_wait();
    }
    FLASH->ADDR = ID_PAGE_ADDR;
    FLASH->CTLR |= FL_STRT;
    flash_wait();
    FLASH->CTLR = 0;

    FLASH->CTLR = FL_LOCK | FL_FLOCK;
}

/* Returns 0 if nothing valid is stored. */
static uint8_t id_load(void)
{
    const volatile uint32_t *p = (const volatile uint32_t *)ID_PAGE_ADDR;
    uint32_t id = p[1];
    if (p[0] == ID_MAGIC && p[2] == ~id && id >= 1 && id <= ID_MAX) {
        return (uint8_t)id;
    }
    return 0;
}

#endif /* DEFAULT_ID == 0 */


/* ---- discovery --------------------------------------------------------- */

void discovery_init(void)
{
    my_uid = be64((const uint8_t *)0x1FFFF7E8);
#if DEFAULT_ID == 0
    slave_id = id_load();
#endif
}

void handle_broadcast(void)
{
    switch (rx_buf[1]) {
    case FN_COMPARE:
        if (rx_len == LEN_COMPARE && slave_id == ID_BROADCAST && my_uid <= be64(&rx_buf[2])) {
            uint8_t b = REPLY_BYTE;
            tx_frame(&b, 1);
        }
        break;
#if DEFAULT_ID == 0
    /* Fixed-ID builds don't take part in address assignment. */
    case FN_ASSIGN:
        /* No reply. The master confirms by reading reg 0 at the new address. */
        if (rx_len == LEN_ASSIGN && be64(&rx_buf[2]) == my_uid &&
            rx_buf[10] >= 1 && rx_buf[10] <= ID_MAX) {
            slave_id = rx_buf[10];
            id_save(slave_id);
        }
        break;
    case FN_FORGET:
        if (rx_len == LEN_FORGET && be64(&rx_buf[2]) == my_uid) {
            slave_id = DEFAULT_ID;
            id_save(0);
        }
        break;
#endif
    default:
        break;
    }
}
