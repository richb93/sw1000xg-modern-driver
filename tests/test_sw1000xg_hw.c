#include "../src/hardware/sw1000xg_hw.h"
#include <assert.h>
#include <stdio.h>

typedef struct event { uint32_t offset, value; } event;
typedef struct fake_mmio {
    event events[8192];
    size_t count;
    uint32_t delays[8];
    size_t delay_count;
    int busy;
} fake_mmio;

static uint32_t fake_read(void *context, uint32_t offset)
{
    fake_mmio *fake = context;
    (void)offset;
    return fake->busy ? SWXG_DSP_BUSY : 0;
}

static void fake_write(void *context, uint32_t offset, uint32_t value)
{
    fake_mmio *fake = context;
    assert(fake->count < sizeof(fake->events) / sizeof(fake->events[0]));
    fake->events[fake->count++] = (event){offset, value};
}

static void fake_delay(void *context, uint32_t milliseconds)
{
    fake_mmio *fake = context;
    assert(fake->delay_count < sizeof(fake->delays) / sizeof(fake->delays[0]));
    fake->delays[fake->delay_count++] = milliseconds;
}

static swxg_device make_device(fake_mmio *fake)
{
    swxg_device device;
    swxg_io io = {fake, fake_read, fake_write, fake_delay};
    swxg_init(&device, io);
    return device;
}

static void test_chunking(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    uint32_t words[33];
    size_t i;
    for (i = 0; i < 33; ++i) words[i] = (uint32_t)i;
    assert(swxg_dsp_send_words(&device, 0, 0x12345, words, 33) == SWXG_OK);
    assert(fake.count == 37);
    assert(fake.events[32].offset == SWXG_DSP0 + 0x80);
    assert(fake.events[32].value == 0x00200001);
    assert(fake.events[33].offset == SWXG_DSP0 + 0x84);
    assert(fake.events[33].value == 0x23450000);
    assert(fake.events[36].value == 0x23650000);
}

static void test_timeout(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    uint32_t word = 0;
    fake.busy = 1;
    device.poll_limit = 2;
    assert(swxg_dsp_send_words(&device, 0, 0, &word, 1) == SWXG_TIMEOUT);
    assert(fake.count == 0);
}

static void test_separate_target_selector(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_dsp_set_word_ex(&device, 0, 0x100, 0xE0,
                                0x40000000) == SWXG_OK);
    assert(fake.count == 3);
    assert(fake.events[1].value == 0x00010000);
    assert(fake.events[2].value == 0x00E00100);
}

static void test_set_ram(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    assert(swxg_set_ram(&device, 0xC101, 0x12345678) == SWXG_OK);
    assert(fake.count == 4);
    assert(fake.events[0].offset == SWXG_DSP0 + 0x80);
    assert(fake.events[0].value == 0x01010000);
    assert(fake.events[1].offset == SWXG_DSP0);
    assert(fake.events[1].value == 0x12345678);
    assert(fake.events[2].value == 0x00010000);
    assert(fake.events[3].offset == SWXG_DSP0 + 0x84);
    assert(fake.events[3].value == 0xC1010F00);
}

static void test_dit_mode1(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    swxg_write_port1(&device, 0xD1A18000);
    fake.count = 0;
    swxg_dit_write(&device, 1, 8);
    /* latch low + 6*(data, clock high, clock low) + latch high */
    assert(fake.count == 20);
    assert((fake.events[0].value & SWXG_PORT1_DIT_LATCH) == 0);
    assert((fake.events[1].value & SWXG_PORT1_DIT_DATA) != 0);
    assert((fake.events[19].value & SWXG_PORT1_DIT_LATCH) != 0);
    assert((fake.events[19].value & SWXG_PORT1_DIT_CLOCK) == 0);
}

static void test_dit_32bit_modes(void)
{
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    swxg_dit_write(&device, 0, 0x80000000);
    assert(fake.count == 104);
    assert((fake.events[1].value & SWXG_PORT1_DIT_DATA) != 0);
    /* Mode 0 trailer is 0,0. */
    assert((fake.events[97].value & SWXG_PORT1_DIT_DATA) == 0);
    assert((fake.events[100].value & SWXG_PORT1_DIT_DATA) == 0);
    fake.count = 0;
    swxg_dit_write(&device, 2, 0);
    assert(fake.count == 104);
    /* Mode 2 trailer is 1,0. */
    assert((fake.events[97].value & SWXG_PORT1_DIT_DATA) != 0);
    assert((fake.events[100].value & SWXG_PORT1_DIT_DATA) == 0);
}

static void test_global(void)
{
    const uint8_t record[18] = {
        0,0,0,0,0,0,0,0, 0x17,0x01, 0x1e,0x01, 0,0, 0,0, 0,0
    };
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    swxg_write_global_record(&device, 0, record);
    assert(fake.count == 3);
    assert(fake.events[0].offset == SWXG_DSP0 + 0x88);
    assert(fake.events[0].value == 0x0117011E);
    assert(fake.events[1].offset == SWXG_DSP0 + 0x8C);
    assert(fake.events[2].offset == SWXG_DSP0 + 0x90);
}

static void test_complete_startup(void)
{
    static const uint8_t global[18] = {0};
    static const uint32_t words[0x140] = {0};
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    swxg_startup_assets assets = {0};
    size_t i;
    for (i = 0; i < 5; ++i) assets.global_records[i] = global;
    for (i = 0; i < 11; ++i) assets.mpr[i] = words;
    assets.bootstrap_zero_a = words;
    assets.bootstrap_zero_b = words;
    assets.cescr = words;
    assert(swxg_startup(&device, &assets) == SWXG_OK);
    assert(fake.delay_count == 2);
    assert(fake.delays[0] == 10 && fake.delays[1] == 44);
    assert(fake.events[0].offset == SWXG_PORT0);
    assert(fake.events[0].value == 0x00000080);
    assert(fake.events[1].offset == SWXG_PORT1);
    assert(fake.events[1].value == 0x11A18000);
    assert(fake.events[fake.count - 1].offset == SWXG_TRPIF);
    assert(fake.events[fake.count - 1].value == 0);
    assert(device.port1_shadow ==
           ((0xD1A18000u | SWXG_PORT1_DIT_LATCH) &
            ~SWXG_PORT1_DIT_CLOCK));
}

int main(void)
{
    test_chunking();
    test_timeout();
    test_separate_target_selector();
    test_set_ram();
    test_dit_mode1();
    test_dit_32bit_modes();
    test_global();
    test_complete_startup();
    puts("sw1000xg_hw tests passed");
    return 0;
}
