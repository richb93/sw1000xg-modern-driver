#include "../src/hardware/sw1000xg_uart.h"
#include <assert.h>
#include <stdio.h>
#include <string.h>

enum { EV_W8 = 1, EV_R8, EV_US, EV_W32 };

typedef struct event { int kind; uint32_t offset, value; } event;

typedef struct fake_uart {
    event events[4096];
    size_t count;
    const uint8_t *status;           /* scripted reads of base+1 */
    size_t status_length, status_pos;
    uint8_t status_default;
    const uint8_t *rx;               /* scripted reads of base+0 */
    size_t rx_length, rx_pos;
} fake_uart;

static void log_event(fake_uart *fake, int kind, uint32_t offset,
                      uint32_t value)
{
    assert(fake->count < sizeof(fake->events) / sizeof(fake->events[0]));
    fake->events[fake->count++] = (event){kind, offset, value};
}

static uint32_t read32(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    return 0;
}

static void write32(void *context, uint32_t offset, uint32_t value)
{
    log_event(context, EV_W32, offset, value);
}

static void delay_ms(void *context, uint32_t milliseconds)
{
    (void)context;
    (void)milliseconds;
}

static uint8_t read8(void *context, uint32_t offset)
{
    fake_uart *fake = context;
    uint8_t value;
    if (offset & 1)
        value = fake->status_pos < fake->status_length
                    ? fake->status[fake->status_pos++]
                    : fake->status_default;
    else
        value = fake->rx_pos < fake->rx_length ? fake->rx[fake->rx_pos++] : 0;
    log_event(fake, EV_R8, offset, value);
    return value;
}

static void write8(void *context, uint32_t offset, uint8_t value)
{
    log_event(context, EV_W8, offset, value);
}

static void delay_us(void *context, uint32_t microseconds)
{
    log_event(context, EV_US, 0, microseconds);
}

static swxg_device make_device(fake_uart *fake)
{
    swxg_device device;
    swxg_io io = {fake, read32, write32, delay_ms, read8, write8, delay_us};
    swxg_init(&device, io);
    return device;
}

static swxg_uart_channel channel;   /* large: keep off the stack */

/* Drains the TX ring into out[] as if the transmitter were always ready. */
static size_t drain(const swxg_uart_channel *ch, uint8_t *out, size_t max)
{
    size_t n = 0;
    uint32_t head = ch->tx_head;
    while (head != ch->tx_tail && n < max) {
        out[n++] = ch->tx[head];
        head = (head + 1) & (SWXG_UART_TX_RING - 1);
    }
    return n;
}

static void test_init_and_enable(void)
{
    static const uint8_t expected[6] = {0x00, 0x00, 0x00, 0x50, 0x4E, 0x10};
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    size_t i;
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    assert(channel.base == 0x3E002);
    assert(fake.count == 12);
    for (i = 0; i < 6; ++i) {
        assert(fake.events[2 * i].kind == EV_W8);
        assert(fake.events[2 * i].offset == 0x3E003);
        assert(fake.events[2 * i].value == expected[i]);
        assert(fake.events[2 * i + 1].kind == EV_US);
        assert(fake.events[2 * i + 1].value == 36);
    }
    swxg_uart_enable_tx(&device, &channel, 1);
    assert(fake.events[12].value == 0x11);
    swxg_uart_enable_rx(&device, &channel, 1);
    assert(fake.events[14].value == 0x15);
    swxg_uart_enable_tx(&device, &channel, 0);
    assert(fake.events[16].value == 0x14);
    assert(swxg_uart_tx_irq_bit(1) == 26 && swxg_uart_rx_irq_bit(0) == 25);
}

static void test_init_needs_byte_io(void)
{
    swxg_device device;
    swxg_io io = {0, read32, write32, delay_ms, 0, 0, 0};
    swxg_init(&device, io);
    assert(swxg_uart_init(&device, &channel, 0) == SWXG_INVALID_ARGUMENT);
    assert(swxg_uart_init(&device, &channel, 2) == SWXG_INVALID_ARGUMENT);
}

static void test_f5_framing(void)
{
    static const uint8_t note_on[3] = {0x90, 0x3C, 0x40};
    static const uint8_t note_on2[3] = {0x91, 0x40, 0x50};
    static const uint8_t data_only[2] = {0x3E, 0x40};
    static const uint8_t expected[] = {
        0xF5, 0x01, 0x90, 0x3C, 0x40,        /* first write selects SWXG1 */
        0x90, 0x3C, 0x40,                    /* same port: no selector */
        0xF5, 0x02, 0x91, 0x40, 0x50,        /* switch to SWXG2 */
        0xF5, 0x01, 0x90, 0x3E, 0x40         /* back: status restored */
    };
    uint8_t out[64];
    swxg_midi_port swxg1, swxg2;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_midi_port_init(&swxg1, 0);
    swxg_midi_port_init(&swxg2, 1);
    assert(swxg_midi_write(&channel, &swxg1, note_on, 3) == SWXG_OK);
    assert(swxg_midi_write(&channel, &swxg1, note_on, 3) == SWXG_OK);
    assert(swxg_midi_write(&channel, &swxg2, note_on2, 3) == SWXG_OK);
    assert(swxg_midi_write(&channel, &swxg1, data_only, 2) == SWXG_OK);
    assert(drain(&channel, out, sizeof(out)) == sizeof(expected));
    assert(memcmp(out, expected, sizeof(expected)) == 0);
    assert(channel.tx_active);
}

static void test_f5_refresh_every_100(void)
{
    static const uint8_t clock = 0xF8;
    swxg_midi_port swxg1;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    uint8_t out[512];
    size_t n, i, selectors = 0;
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_midi_port_init(&swxg1, 0);
    for (i = 0; i < 201; ++i)
        assert(swxg_midi_write(&channel, &swxg1, &clock, 1) == SWXG_OK);
    n = drain(&channel, out, sizeof(out));
    for (i = 0; i < n; ++i)
        if (out[i] == 0xF5) ++selectors;
    /* Writes 1, 101 and 201 carry a selector, as with Yamaha's IsNeedF5. */
    assert(selectors == 3);
    assert(n == 201 + 3 * 2);
    assert(out[0] == 0xF5 && out[2] == 0xF8);
    assert(out[2 + 100] == 0xF5);
}

static void test_external_port_and_running_status(void)
{
    static const uint8_t mixed[] = {0x92, 0x10, 0x20, 0xF8, 0xB3, 0x07};
    static const uint8_t sysex[] = {0xF0, 0x43, 0x10, 0x4C, 0, 0, 0x7E, 0,
                                    0xF7};
    static const uint8_t data_only[1] = {0x55};
    uint8_t out[64];
    swxg_midi_port external, swxg3, swxg1;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_uart_init(&device, &channel, SWXG_UART_EXTERNAL) == SWXG_OK);
    swxg_midi_port_init(&external, SWXG_MIDI_NO_PORT);
    assert(swxg_midi_write(&channel, &external, mixed, sizeof(mixed)) ==
           SWXG_OK);
    assert(drain(&channel, out, sizeof(out)) == sizeof(mixed));
    assert(external.running_status == 0xB3);  /* last channel status wins */
    assert(swxg_midi_write(&channel, &external, sysex, sizeof(sysex)) ==
           SWXG_OK);
    assert(external.running_status == 0);     /* SysEx cancels it */

    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_midi_port_init(&swxg3, 2);
    swxg_midi_port_init(&swxg1, 0);
    assert(swxg_midi_write(&channel, &swxg3, data_only, 1) == SWXG_OK);
    assert(swxg_midi_write(&channel, &swxg1, mixed, 3) == SWXG_OK);
    assert(drain(&channel, out, sizeof(out)) == 3 + 5);
    /* No running status known yet: nothing is invented. */
    assert(out[0] == 0xF5 && out[1] == 0x03 && out[2] == 0x55);
}

static void test_queue_full_is_all_or_nothing(void)
{
    static uint8_t big[SWXG_UART_TX_RING];
    static const uint8_t note[3] = {0x90, 0x3C, 0x40};
    swxg_midi_port swxg1;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    uint32_t tail;
    size_t fill = SWXG_UART_TX_RING - 1 - 2 - 2;   /* leave 2 bytes free */
    memset(big, 0x40, sizeof(big));
    big[0] = 0x90;
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_midi_port_init(&swxg1, 0);
    assert(swxg_midi_write(&channel, &swxg1, big, fill) == SWXG_OK);
    assert(swxg_uart_tx_free(&channel) == 2);
    tail = channel.tx_tail;
    assert(swxg_midi_write(&channel, &swxg1, note, 3) == SWXG_QUEUE_FULL);
    assert(channel.tx_tail == tail);
    assert(channel.refresh == SWXG_MIDI_F5_REFRESH);  /* tracker untouched */
    assert(swxg_midi_write(&channel, &swxg1, big, SWXG_UART_TX_RING) ==
           SWXG_QUEUE_FULL);
    assert(swxg_midi_write(&channel, &swxg1, note, 2) == SWXG_OK);
    assert(swxg_uart_tx_free(&channel) == 0);
    assert(swxg_midi_write(&channel, &swxg1, note, 0) == SWXG_OK);
    assert(swxg_midi_write(&channel, &swxg1, NULL, 1) ==
           SWXG_INVALID_ARGUMENT);
}

static void test_service_tx(void)
{
    static const uint8_t note[3] = {0x90, 0x3C, 0x40};
    /* ready, not ready, ready with an error bit, ready, ready (empty) */
    static const uint8_t status[] = {0x01, 0x00, 0x09, 0x01, 0x01};
    swxg_midi_port swxg1;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    size_t start, i, writes = 0;
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_uart_enable_tx(&device, &channel, 1);
    swxg_midi_port_init(&swxg1, 0);
    /* One queued byte is enough to walk every branch. */
    assert(swxg_midi_write(&channel, &swxg1, note, 1) == SWXG_OK);
    assert(swxg_uart_tx_pending(&channel));
    fake.status = status;
    fake.status_length = sizeof(status);
    start = fake.count;
    assert(swxg_uart_service_tx(&device, &channel) == 1);   /* sends F5 */
    assert(swxg_uart_service_tx(&device, &channel) == 1);   /* not ready */
    assert(swxg_uart_service_tx(&device, &channel) == 1);   /* error+send */
    assert(channel.errors == 1);
    assert(swxg_uart_service_tx(&device, &channel) == 1);   /* last byte */
    assert(swxg_uart_service_tx(&device, &channel) == 0);   /* drained */
    assert(!channel.tx_active && !swxg_uart_tx_pending(&channel));
    for (i = start; i < fake.count; ++i) {
        if (fake.events[i].kind == EV_W8 && fake.events[i].offset == 0x3E003)
            assert(fake.events[i].value == 0x11);   /* 0x11 | 0x10 */
        if (fake.events[i].kind == EV_W8 && fake.events[i].offset == 0x3E002)
            ++writes;
    }
    assert(writes == 3);   /* F5 01 90 */
}

static void test_service_rx_and_overflow(void)
{
    static const uint8_t status[] = {0x02, 0x02, 0x02, 0x00};
    static const uint8_t bytes[] = {0x90, 0x3C, 0x40};
    uint8_t out[8];
    size_t i;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_uart_init(&device, &channel, SWXG_UART_EXTERNAL) == SWXG_OK);
    fake.status = status;
    fake.status_length = sizeof(status);
    fake.rx = bytes;
    fake.rx_length = sizeof(bytes);
    assert(swxg_uart_service_rx(&device, &channel) == 3);
    assert(swxg_uart_read(&channel, out, 2) == 2);
    assert(out[0] == 0x90 && out[1] == 0x3C);
    assert(swxg_uart_read(&channel, out, 8) == 1 && out[0] == 0x40);
    assert(swxg_uart_read(&channel, out, 8) == 0);

    /* A receiver stuck at "ready" is bounded and overflow is counted. */
    fake.status = 0;
    fake.status_default = 0x02;
    assert(swxg_uart_service_rx(&device, &channel) == SWXG_UART_RX_RING);
    assert(channel.rx_overflow == 1);
    for (i = 0; i < SWXG_UART_RX_RING - 1; ++i)
        assert(swxg_uart_read(&channel, out, 1) == 1);
    assert(swxg_uart_read(&channel, out, 1) == 0);
}

static void test_poll_tx(void)
{
    static const uint8_t note[3] = {0x90, 0x3C, 0x40};
    swxg_midi_port swxg1;
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_uart_init(&device, &channel, SWXG_UART_SWXG) == SWXG_OK);
    swxg_midi_port_init(&swxg1, 0);
    assert(swxg_midi_write(&channel, &swxg1, note, 3) == SWXG_OK);
    /* Never ready: bounded by max_polls. */
    assert(swxg_uart_poll_tx(&device, &channel, 100, 10) == 0);
    fake.status_default = 0x01;
    assert(swxg_uart_poll_tx(&device, &channel, 2, 1000) == 2);
    assert(swxg_uart_poll_tx(&device, &channel, 100, 1000) == 3);
    assert(!swxg_uart_tx_pending(&channel) && !channel.tx_active);
}

static void test_irq_shadow(void)
{
    fake_uart fake = {0};
    swxg_device device = make_device(&fake);
    swxg_irq_set(&device, 26, 1);
    swxg_irq_set(&device, 25, 1);
    swxg_irq_set(&device, 26, 0);
    swxg_irq_set(&device, 32, 1);            /* ignored */
    assert(fake.count == 3);
    assert(fake.events[0].offset == SWXG_TRPIF);
    assert(fake.events[0].value == 1u << 26);
    assert(fake.events[1].value == ((1u << 26) | (1u << 25)));
    assert(fake.events[2].value == 1u << 25);
}

int main(void)
{
    test_init_and_enable();
    test_init_needs_byte_io();
    test_f5_framing();
    test_f5_refresh_every_100();
    test_external_port_and_running_status();
    test_queue_full_is_all_or_nothing();
    test_service_tx();
    test_service_rx_and_overflow();
    test_poll_tx();
    test_irq_shadow();
    puts("sw1000xg_uart tests passed");
    return 0;
}
