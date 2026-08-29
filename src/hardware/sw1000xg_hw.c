#include "sw1000xg_hw.h"

static const uint32_t dsp_windows[5] = {
    0x3F000, 0x3F100, 0x3F200, 0x3F300, 0x3F400
};

static int wait_ready(swxg_device *device, uint32_t window)
{
    uint32_t remaining = device->poll_limit;
    while ((device->io.read32(device->io.context, window + 0x80) &
            SWXG_DSP_BUSY) != 0) {
        if (remaining-- == 0)
            return SWXG_TIMEOUT;
    }
    return SWXG_OK;
}

static int16_t le_i16(const uint8_t *p)
{
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

void swxg_init(swxg_device *device, swxg_io io)
{
    device->io = io;
    device->port1_shadow = 0;
    device->poll_limit = 0xFFFFFu;
}

void swxg_write_port1(swxg_device *device, uint32_t value)
{
    device->port1_shadow = value;
    device->io.write32(device->io.context, SWXG_PORT1, value);
}

void swxg_set_port1_bit(swxg_device *device, uint32_t mask, int enabled)
{
    uint32_t value = enabled ? device->port1_shadow | mask
                             : device->port1_shadow & ~mask;
    /* Yamaha's helpers preserve bit 31 from the old shadow explicitly. */
    value = (value & 0x7FFFFFFFu) | (device->port1_shadow & 0x80000000u);
    swxg_write_port1(device, value);
}

int swxg_dsp_send_words_ex(swxg_device *device, uint16_t window_index,
                           uint16_t target_selector, uint32_t destination,
                           const uint32_t *words, uint16_t word_count)
{
    uint32_t window;
    if (!device || !device->io.read32 || !device->io.write32 ||
        window_index >= 5 || (!words && word_count != 0))
        return SWXG_INVALID_ARGUMENT;
    window = dsp_windows[window_index];
    while (word_count != 0) {
        uint16_t i;
        uint16_t chunk = word_count > 32 ? 32 : word_count;
        int result = wait_ready(device, window);
        if (result != SWXG_OK)
            return result;
        for (i = 0; i < chunk; ++i)
            device->io.write32(device->io.context, window + (uint32_t)i * 4,
                               words[i]);
        device->io.write32(device->io.context, window + 0x80,
                           ((uint32_t)chunk << 16) | (destination >> 16));
        device->io.write32(device->io.context, window + 0x84,
                           (destination << 16) | target_selector);
        words += chunk;
        destination += chunk;
        word_count = (uint16_t)(word_count - chunk);
    }
    return SWXG_OK;
}

int swxg_dsp_send_words(swxg_device *device, uint16_t dsp_index,
                        uint32_t destination, const uint32_t *words,
                        uint16_t word_count)
{
    return swxg_dsp_send_words_ex(device, dsp_index, dsp_index, destination,
                                  words, word_count);
}

int swxg_dsp_set_word(swxg_device *device, uint16_t dsp_index,
                      uint32_t destination, uint32_t value)
{
    return swxg_dsp_send_words(device, dsp_index, destination, &value, 1);
}

int swxg_dsp_set_word_ex(swxg_device *device, uint16_t window_index,
                         uint16_t target_selector, uint32_t destination,
                         uint32_t value)
{
    return swxg_dsp_send_words_ex(device, window_index, target_selector,
                                  destination, &value, 1);
}

int swxg_set_ram(swxg_device *device, uint32_t address, uint32_t value)
{
    uint32_t high;
    int result;
    if (!device || !device->io.read32 || !device->io.write32)
        return SWXG_INVALID_ARGUMENT;
    result = wait_ready(device, SWXG_DSP0);
    if (result != SWXG_OK)
        return result;
    high = address >> 16;
    device->io.write32(device->io.context, SWXG_DSP0 + 0x80,
                       0x01010000u + high);
    device->io.write32(device->io.context, SWXG_DSP0, value);
    device->io.write32(device->io.context, SWXG_DSP0 + 0x80,
                       0x00010000u + high);
    device->io.write32(device->io.context, SWXG_DSP0 + 0x84,
                       (address << 16) + 0x0F00u);
    return SWXG_OK;
}

static void dit_clock(swxg_device *device)
{
    swxg_set_port1_bit(device, SWXG_PORT1_DIT_CLOCK, 1);
    swxg_set_port1_bit(device, SWXG_PORT1_DIT_CLOCK, 0);
}

static void dit_bit(swxg_device *device, int bit)
{
    swxg_set_port1_bit(device, SWXG_PORT1_DIT_DATA, bit);
    dit_clock(device);
}

void swxg_dit_write(swxg_device *device, uint8_t mode, uint32_t value)
{
    uint32_t mask;
    swxg_set_port1_bit(device, SWXG_PORT1_DIT_LATCH, 0);
    if (mode == 0 || mode == 2) {
        for (mask = 0x80000000u; mask != 0; mask >>= 1)
            dit_bit(device, (value & mask) != 0);
        dit_bit(device, mode == 2);
        dit_bit(device, 0);
    } else if (mode == 1) {
        for (mask = 8; mask != 0; mask >>= 1)
            dit_bit(device, (value & mask) != 0);
        dit_bit(device, 0);
        dit_bit(device, 1);
    }
    swxg_set_port1_bit(device, SWXG_PORT1_DIT_LATCH, 1);
}

void swxg_write_global_record(swxg_device *device, uint16_t dsp_index,
                              const uint8_t record[18])
{
    uint32_t base;
    uint32_t first;
    uint32_t second;
    uint32_t third;
    if (!device || !record || dsp_index >= 5)
        return;
    base = dsp_windows[dsp_index];
    first = ((uint32_t)(int32_t)le_i16(record + 8) << 16) +
            (uint32_t)(int32_t)le_i16(record + 10);
    second = (uint32_t)(int32_t)le_i16(record + 14);
    third = (uint32_t)(int32_t)le_i16(record + 16) << 16;
    device->io.write32(device->io.context, base + 0x88, first);
    device->io.write32(device->io.context, base + 0x8C, second);
    device->io.write32(device->io.context, base + 0x90, third);
}

int swxg_startup(swxg_device *device, const swxg_startup_assets *assets)
{
    static const uint16_t mpr_words[11] = {
        0x140, 0x140, 0x140, 0x140, 0x0A0, 0x140,
        0x140, 0x040, 0x006, 0x020, 0x040
    };
    size_t i;
    int result;
    if (!device || !assets || !device->io.delay_ms ||
        !assets->bootstrap_zero_a || !assets->bootstrap_zero_b ||
        !assets->cescr)
        return SWXG_INVALID_ARGUMENT;
    for (i = 0; i < 5; ++i)
        if (!assets->global_records[i]) return SWXG_INVALID_ARGUMENT;
    for (i = 0; i < 11; ++i)
        if (!assets->mpr[i]) return SWXG_INVALID_ARGUMENT;

    device->io.write32(device->io.context, SWXG_PORT0, 0x00000080u);
    swxg_write_port1(device, 0x11A18000u);
    device->io.delay_ms(device->io.context, 10);

    swxg_set_port1_bit(device, 1u << 25, 1); /* SetMute(1) */
    swxg_set_port1_bit(device, 1u << 23, 0); /* NResDSP0 = 0 */
    device->io.delay_ms(device->io.context, 44);
    swxg_set_port1_bit(device, 1u << 23, 1); /* NResDSP0 = 1 */
    swxg_write_port1(device, device->port1_shadow & 0x3FFFFFFFu);

    for (i = 0; i < 5; ++i)
        swxg_write_global_record(device, (uint16_t)i,
                                 assets->global_records[i]);
    result = swxg_dsp_send_words_ex(device, 0, 0, 0x700,
                                    assets->bootstrap_zero_a, 64);
    if (result != SWXG_OK) return result;
    for (i = 0; i < 11; ++i) {
        result = swxg_dsp_send_words_ex(device, 0, 0, 0,
                                        assets->mpr[i], mpr_words[i]);
        if (result != SWXG_OK) return result;
    }
    result = swxg_dsp_send_words_ex(device, 0, 0, 0x700,
                                    assets->bootstrap_zero_b, 64);
    if (result != SWXG_OK) return result;
    result = swxg_dsp_send_words_ex(device, 0, 0, 0x800, assets->cescr, 6);
    if (result != SWXG_OK) return result;
    result = swxg_set_ram(device, 0xC100, 0);
    if (result != SWXG_OK) return result;
    result = swxg_set_ram(device, 0xC101, 0);
    if (result != SWXG_OK) return result;
    swxg_set_port1_bit(device, 1u << 25, 0);
    result = swxg_dsp_set_word_ex(device, 0, 0x100, 0xE0, 0x40000000);
    if (result != SWXG_OK) return result;
    result = swxg_dsp_set_word_ex(device, 0, 0x700, 0x0F, 0x147F0020);
    if (result != SWXG_OK) return result;

    swxg_write_port1(device, 0xD1A18000u);
    swxg_dit_write(device, 0, 0x0204);
    swxg_dit_write(device, 1, 0x0008);
    swxg_dit_write(device, 2, 0x0000);
    device->io.write32(device->io.context, SWXG_TRPIF, 0);
    return SWXG_OK;
}
