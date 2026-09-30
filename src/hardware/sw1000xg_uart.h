#ifndef SW1000XG_UART_H
#define SW1000XG_UART_H

#include "sw1000xg_hw.h"

/*
 * PCI-UART layer: channel init and commands, transmit/receive rings,
 * interrupt-time servicing, and Yamaha's logical-port framing (F5 <port+1>)
 * for the SWXG endpoints that share UART 1.
 *
 * Concurrency is the caller's job:
 *  - swxg_midi_write() runs under a per-channel lock (the producer side of
 *    the TX ring). Afterwards the caller enables the channel's TX interrupt.
 *  - swxg_uart_service_tx()/service_rx() run in the ISR, or in an
 *    interrupt-synchronised routine (the consumer of TX, producer of RX).
 *  - swxg_uart_read() is the RX consumer.
 * The rings are single-producer/single-consumer.
 */

enum {
    SWXG_UART_EXTERNAL = 0,          /* mini-DIN MIDI OUT/IN */
    SWXG_UART_SWXG = 1,              /* shared SWXG1-3 transport */
    SWXG_UART_COUNT = 2
};

enum {
    SWXG_UART_STAT_TX_READY = 0x01,
    SWXG_UART_STAT_RX_READY = 0x02,
    SWXG_UART_STAT_TX_EMPTY = 0x04,
    SWXG_UART_STAT_ERROR = 0x38,

    SWXG_UART_CMD_TX_ENABLE = 0x01,
    SWXG_UART_CMD_RX_ENABLE = 0x04,
    SWXG_UART_CMD_ERROR_RESET = 0x10,

    SWXG_UART_COMMAND_DELAY_US = 36
};

#define SWXG_UART_TX_RING 8192u      /* holds 8191 bytes */
#define SWXG_UART_RX_RING 1024u      /* holds 1023 bytes */
#define SWXG_MIDI_NO_PORT 0xFFu
#define SWXG_MIDI_F5_REFRESH 100u

typedef struct swxg_uart_channel {
    uint32_t base;                   /* BAR offset: data +0, command/status +1 */
    uint8_t command;                 /* shadow of the last command byte */
    uint8_t tx_active;               /* TX interrupt should stay enabled */
    uint8_t selected_port;           /* last F5 logical port, or NO_PORT */
    uint8_t refresh;                 /* writes left before a forced F5 */
    uint32_t errors;                 /* status reads with error bits set */
    uint32_t rx_overflow;            /* received bytes dropped */
    uint32_t tx_head, tx_tail;
    uint32_t rx_head, rx_tail;
    uint8_t tx[SWXG_UART_TX_RING];
    uint8_t rx[SWXG_UART_RX_RING];
} swxg_uart_channel;

typedef struct swxg_midi_port {
    uint8_t logical;                 /* SWXG1-3: 0-2 (sends F5 logical+1);
                                        SWXG_MIDI_NO_PORT for external MIDI */
    uint8_t running_status;          /* last channel status written, or 0 */
} swxg_midi_port;

uint32_t swxg_uart_tx_irq_bit(unsigned index);
uint32_t swxg_uart_rx_irq_bit(unsigned index);

/* Sends 00 00 00 50 4E 10 and clears both rings; needs read8/write8/delay_us. */
int swxg_uart_init(swxg_device *device, swxg_uart_channel *channel,
                   unsigned index);
void swxg_uart_command(swxg_device *device, swxg_uart_channel *channel,
                       uint8_t command);
void swxg_uart_enable_tx(swxg_device *device, swxg_uart_channel *channel,
                         int enabled);
void swxg_uart_enable_rx(swxg_device *device, swxg_uart_channel *channel,
                         int enabled);
/* Empties the TX ring and forgets the F5 selection (Yamaha's OutClear). */
void swxg_uart_reset_tx(swxg_uart_channel *channel);

void swxg_midi_port_init(swxg_midi_port *port, uint8_t logical);
/* Queues one client write, with any F5 selector and running-status restore,
 * all or nothing. Returns SWXG_OK or SWXG_QUEUE_FULL. */
int swxg_midi_write(swxg_uart_channel *channel, swxg_midi_port *port,
                    const uint8_t *data, size_t length);
size_t swxg_uart_tx_free(const swxg_uart_channel *channel);
int swxg_uart_tx_pending(const swxg_uart_channel *channel);

/* Interrupt-time TX: sends at most one byte. Returns nonzero while the TX
 * interrupt should stay enabled, 0 once the ring is drained (mask it). */
int swxg_uart_service_tx(swxg_device *device, swxg_uart_channel *channel);
/* Interrupt-time RX: drains the receiver. Returns bytes received. */
size_t swxg_uart_service_rx(swxg_device *device, swxg_uart_channel *channel);
size_t swxg_uart_read(swxg_uart_channel *channel, uint8_t *buffer,
                      size_t length);
/* Polled TX for bring-up without interrupts: sends while the transmitter is
 * ready, at most max_bytes and max_polls status reads. Returns bytes sent. */
size_t swxg_uart_poll_tx(swxg_device *device, swxg_uart_channel *channel,
                         size_t max_bytes, uint32_t max_polls);

#endif
