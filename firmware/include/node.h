#pragma once
#include "config.h"

#define NUM_REGS      4
#define RX_BUF_SIZE   64
#define ID_BROADCAST  0                /* also means "unassigned" */
#define FN_COMPARE    0x41
#define LEN_COMPARE   12

/* modbus.c */
extern uint16_t regs[NUM_REGS];        /* what the master sees */
extern uint16_t reg_max[NUM_REGS];     /* highest writable value, 0 = read-only */
extern uint8_t  rx_buf[RX_BUF_SIZE];
extern uint8_t  rx_len;
extern uint8_t  rx_overflow;
extern uint8_t  slave_id;

void uart_init(void);
int  frame_poll(void);
int  crc_good(void);
void tx_frame(const uint8_t *p, uint8_t n);
void handle_frame(void);

/* discovery.c */
extern uint64_t my_uid;

void discovery_init(void);
void handle_broadcast(void);

/* flavors.c */
void flavor_init(void);
void flavor_poll(void);
