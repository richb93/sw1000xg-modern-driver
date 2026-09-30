#include "sw1000xg_uart.h"

static const uint32_t uart_base[SWXG_UART_COUNT] = { 0x3E000, 0x3E002 };

static uint32_t tx_count(const swxg_uart_channel *channel)
{
    return (channel->tx_tail - channel->tx_head) & (SWXG_UART_TX_RING - 1);
}

uint32_t swxg_uart_tx_irq_bit(unsigned index)
{
    return 24u + 2u * index;
}

uint32_t swxg_uart_rx_irq_bit(unsigned index)
{
    return 25u + 2u * index;
}

void swxg_uart_command(swxg_device *device, swxg_uart_channel *channel,
                       uint8_t command)
{
    channel->command = command;
    device->io.write8(device->io.context, channel->base + 1, command);
    device->io.delay_us(device->io.context, SWXG_UART_COMMAND_DELAY_US);
}

/* Yamaha's _GetStatUART: error bits are answered with command | 0x10. */
static uint8_t read_status(swxg_device *device, swxg_uart_channel *channel)
{
    uint8_t status = device->io.read8(device->io.context, channel->base + 1);
    if (status & SWXG_UART_STAT_ERROR) {
        ++channel->errors;
        swxg_uart_command(device, channel,
                          (uint8_t)(channel->command |
                                    SWXG_UART_CMD_ERROR_RESET));
    }
    return status;
}

void swxg_uart_reset_tx(swxg_uart_channel *channel)
{
    channel->tx_head = channel->tx_tail = 0;
    channel->tx_active = 0;
    channel->selected_port = SWXG_MIDI_NO_PORT;
    channel->refresh = SWXG_MIDI_F5_REFRESH;
}

int swxg_uart_init(swxg_device *device, swxg_uart_channel *channel,
                   unsigned index)
{
    static const uint8_t sequence[6] = { 0x00, 0x00, 0x00, 0x50, 0x4E, 0x10 };
    size_t i;
    if (!device || !channel || index >= SWXG_UART_COUNT ||
        !device->io.read8 || !device->io.write8 || !device->io.delay_us)
        return SWXG_INVALID_ARGUMENT;
    channel->base = uart_base[index];
    channel->errors = 0;
    channel->rx_overflow = 0;
    channel->rx_head = channel->rx_tail = 0;
    swxg_uart_reset_tx(channel);
    for (i = 0; i < sizeof(sequence); ++i)
        swxg_uart_command(device, channel, sequence[i]);
    return SWXG_OK;
}

void swxg_uart_enable_tx(swxg_device *device, swxg_uart_channel *channel,
                         int enabled)
{
    swxg_uart_command(device, channel, (uint8_t)(enabled
        ? channel->command | SWXG_UART_CMD_TX_ENABLE
        : channel->command & ~SWXG_UART_CMD_TX_ENABLE));
}

void swxg_uart_enable_rx(swxg_device *device, swxg_uart_channel *channel,
                         int enabled)
{
    swxg_uart_command(device, channel, (uint8_t)(enabled
        ? channel->command | SWXG_UART_CMD_RX_ENABLE
        : channel->command & ~SWXG_UART_CMD_RX_ENABLE));
}

void swxg_midi_port_init(swxg_midi_port *port, uint8_t logical)
{
    port->logical = logical;
    port->running_status = 0;
}

size_t swxg_uart_tx_free(const swxg_uart_channel *channel)
{
    return SWXG_UART_TX_RING - 1 - tx_count(channel);
}

int swxg_uart_tx_pending(const swxg_uart_channel *channel)
{
    return tx_count(channel) != 0;
}

static void tx_put(swxg_uart_channel *channel, uint8_t byte)
{
    channel->tx[channel->tx_tail] = byte;
    channel->tx_tail = (channel->tx_tail + 1) & (SWXG_UART_TX_RING - 1);
}

int swxg_midi_write(swxg_uart_channel *channel, swxg_midi_port *port,
                    const uint8_t *data, size_t length)
{
    uint8_t prefix[3];
    size_t prefix_length = 0;
    size_t i;
    int select = 0;
    if (!channel || !port || (!data && length != 0))
        return SWXG_INVALID_ARGUMENT;
    if (length == 0)
        return SWXG_OK;

    if (port->logical != SWXG_MIDI_NO_PORT) {
        /* Yamaha's IsNeedF5: a port switch, or every 100th write. */
        select = channel->selected_port != port->logical ||
                 channel->refresh == 1;
        if (select) {
            prefix[prefix_length++] = 0xF5;
            prefix[prefix_length++] = (uint8_t)(port->logical + 1);
            /* A chunk that continues a message needs its status again. */
            if (!(data[0] & 0x80) && port->running_status != 0)
                prefix[prefix_length++] = port->running_status;
        }
    }
    if (length > swxg_uart_tx_free(channel) ||
        prefix_length > swxg_uart_tx_free(channel) - length)
        return SWXG_QUEUE_FULL;

    if (port->logical != SWXG_MIDI_NO_PORT) {
        channel->selected_port = port->logical;
        channel->refresh = select ? SWXG_MIDI_F5_REFRESH
                                  : (uint8_t)(channel->refresh - 1);
    }
    for (i = 0; i < prefix_length; ++i)
        tx_put(channel, prefix[i]);
    for (i = 0; i < length; ++i) {
        uint8_t byte = data[i];
        tx_put(channel, byte);
        if (byte >= 0x80 && byte <= 0xEF)
            port->running_status = byte;
        else if (byte >= 0xF0 && byte <= 0xF7)
            port->running_status = 0;   /* real-time F8-FF leave it alone */
    }
    channel->tx_active = 1;
    return SWXG_OK;
}

int swxg_uart_service_tx(swxg_device *device, swxg_uart_channel *channel)
{
    uint8_t status = read_status(device, channel);
    if (status & SWXG_UART_STAT_TX_READY) {
        if (tx_count(channel) == 0) {
            channel->tx_active = 0;
            return 0;
        }
        device->io.write8(device->io.context, channel->base,
                          channel->tx[channel->tx_head]);
        channel->tx_head = (channel->tx_head + 1) & (SWXG_UART_TX_RING - 1);
    }
    return channel->tx_active;
}

size_t swxg_uart_service_rx(swxg_device *device, swxg_uart_channel *channel)
{
    size_t received = 0;
    /* Bounded: a stuck status bit must not hold the ISR forever. */
    while (received < SWXG_UART_RX_RING &&
           (read_status(device, channel) & SWXG_UART_STAT_RX_READY)) {
        uint8_t byte = device->io.read8(device->io.context, channel->base);
        uint32_t next = (channel->rx_tail + 1) & (SWXG_UART_RX_RING - 1);
        ++received;
        if (next == channel->rx_head) {
            ++channel->rx_overflow;
            continue;
        }
        channel->rx[channel->rx_tail] = byte;
        channel->rx_tail = next;
    }
    return received;
}

size_t swxg_uart_read(swxg_uart_channel *channel, uint8_t *buffer,
                      size_t length)
{
    size_t count = 0;
    while (count < length && channel->rx_head != channel->rx_tail) {
        buffer[count++] = channel->rx[channel->rx_head];
        channel->rx_head = (channel->rx_head + 1) & (SWXG_UART_RX_RING - 1);
    }
    return count;
}

size_t swxg_uart_poll_tx(swxg_device *device, swxg_uart_channel *channel,
                         size_t max_bytes, uint32_t max_polls)
{
    size_t sent = 0;
    uint32_t polls = 0;
    while (sent < max_bytes && tx_count(channel) != 0 && polls < max_polls) {
        ++polls;
        if (read_status(device, channel) & SWXG_UART_STAT_TX_READY) {
            device->io.write8(device->io.context, channel->base,
                              channel->tx[channel->tx_head]);
            channel->tx_head =
                (channel->tx_head + 1) & (SWXG_UART_TX_RING - 1);
            ++sent;
        }
    }
    if (tx_count(channel) == 0)
        channel->tx_active = 0;
    return sent;
}
