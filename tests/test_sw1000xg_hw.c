#include "../src/hardware/sw1000xg_hw.h"
#include "../src/hardware/sw1000xg_trace.h"
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
    static const uint16_t mpr_words[11] = {
        0x140, 0x140, 0x140, 0x140, 0x0A0, 0x140,
        0x140, 0x040, 0x006, 0x020, 0x040
    };
    uint32_t expected[128];
    size_t expected_count = 0;
    size_t commit = 0;
    fake_mmio fake = {0};
    swxg_device device = make_device(&fake);
    swxg_startup_assets assets = {0};
    size_t i;
    uint32_t done;
    for (i = 0; i < 5; ++i) assets.global_records[i] = global;
    for (i = 0; i < 11; ++i) assets.mpr[i] = words;
    assets.cescr = words;
    assert(swxg_startup(&device, &assets) == SWXG_OK);
    assert(fake.delay_count == 2);
    assert(fake.delays[0] == 10 && fake.delays[1] == 1);
    assert(fake.events[0].offset == SWXG_PORT0);
    assert(fake.events[0].value == 0x00000080);
    assert(fake.events[1].offset == SWXG_PORT1);
    assert(fake.events[1].value == 0x11A18000);

    /* Every DSP window-0 commit, in order: (destination << 16) | selector. */
    expected[expected_count++] = 0x00400700;          /* n1mod0KeyOn */
    for (i = 0; i < 11; ++i)
        for (done = 0; done < mpr_words[i]; done += 32) {
            assert(expected_count < sizeof(expected) / sizeof(expected[0]));
            expected[expected_count++] = (done << 16) | ((uint32_t)i << 8);
        }
    expected[expected_count++] = 0x00400700;          /* n1mod0KeyOnOff */
    expected[expected_count++] = 0x00000800;          /* CESCR */
    expected[expected_count++] = 0xC1000F00;          /* TRWF */
    expected[expected_count++] = 0xC1010F00;          /* TRWFO */
    expected[expected_count++] = 0x00E00100;          /* dspSetRun(1) */
    for (i = 0; i < fake.count; ++i) {
        assert(fake.events[i].value != 0x147F0020);   /* 16-bit run path */
        if (fake.events[i].offset != SWXG_DSP0 + 0x84)
            continue;
        assert(commit < expected_count);
        assert(fake.events[i].value == expected[commit]);
        if (commit == 0 || commit == expected_count - 5) {
            /* Single-word key writes: data 0xFFFF, then 0. */
            assert(fake.events[i - 2].offset == SWXG_DSP0);
            assert(fake.events[i - 2].value == (commit == 0 ? 0xFFFFu : 0));
        }
        ++commit;
    }
    assert(commit == expected_count);

    assert(fake.events[fake.count - 1].offset == SWXG_TRPIF);
    assert(fake.events[fake.count - 1].value == 0);
    assert(device.port1_shadow ==
           ((0xD1A18000u | SWXG_PORT1_DIT_LATCH) &
            ~SWXG_PORT1_DIT_CLOCK));
}

static uint32_t busy_countdown;

static uint32_t countdown_read(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    if (busy_countdown == 0)
        return 0;
    --busy_countdown;
    return SWXG_DSP_BUSY;
}

static void test_trace(void)
{
    fake_mmio fake = {0};
    swxg_io inner = {&fake, countdown_read, fake_write, fake_delay};
    swxg_trace_entry entries[4];
    swxg_trace trace;
    swxg_device device;
    swxg_trace_init(&trace, inner, entries, 4);
    swxg_init(&device, swxg_trace_io(&trace));
    busy_countdown = 5;
    assert(swxg_set_ram(&device, 0xC100, 7) == SWXG_OK);
    /* 5 busy polls fold into one entry, then the ready read. */
    assert(entries[0].kind == SWXG_TRACE_READ);
    assert(entries[0].value == SWXG_DSP_BUSY && entries[0].repeat == 5);
    assert(entries[1].kind == SWXG_TRACE_READ && entries[1].value == 0);
    assert(entries[2].kind == SWXG_TRACE_WRITE);
    assert(entries[2].offset == SWXG_DSP0 + 0x80);
    /* Capacity 4: the remaining three writes, one fits, two are dropped. */
    assert(trace.count == 4 && trace.dropped == 2);
    /* Every access was still forwarded to the inner io. */
    assert(fake.count == 4);
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
    test_trace();
    puts("sw1000xg_hw tests passed");
    return 0;
}
