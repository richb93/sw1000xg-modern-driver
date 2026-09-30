/* Runs swxg_startup against an idle fake BAR and prints the MMIO trace in
 * the format parsed by tools/recipe_trace.py. Link with
 * tests/zero_assets.c, or with a private sw1000xg_assets.generated.c to get
 * real data words. */
#include "../src/hardware/sw1000xg_trace.h"
#include "../src/kmdf/sw1000xg_assets.generated.h"
#include <stdio.h>

static uint32_t idle_read(void *context, uint32_t offset)
{
    (void)context;
    (void)offset;
    return 0;
}

static void ignore_write(void *context, uint32_t offset, uint32_t value)
{
    (void)context;
    (void)offset;
    (void)value;
}

static void ignore_delay(void *context, uint32_t milliseconds)
{
    (void)context;
    (void)milliseconds;
}

int main(void)
{
    static swxg_trace_entry entries[8192];
    swxg_io bar = {0, idle_read, ignore_write, ignore_delay};
    swxg_trace trace;
    swxg_device device;
    size_t i;
    int result;

    swxg_trace_init(&trace, bar, entries, sizeof(entries) / sizeof(entries[0]));
    swxg_init(&device, swxg_trace_io(&trace));
    result = swxg_startup(&device, SwxgGetStartupAssets());
    for (i = 0; i < trace.count; ++i) {
        const swxg_trace_entry *e = &entries[i];
        if (e->kind == SWXG_TRACE_WRITE)
            printf("SWXG W %05X %08X\n", (unsigned)e->offset,
                   (unsigned)e->value);
        else if (e->kind == SWXG_TRACE_READ)
            printf("SWXG R %05X %08X %u\n", (unsigned)e->offset,
                   (unsigned)e->value, (unsigned)e->repeat);
        else
            printf("SWXG D %u\n", (unsigned)e->value);
    }
    printf("SWXG END %d %u\n", result, (unsigned)trace.dropped);
    return result == SWXG_OK && trace.dropped == 0 ? 0 : 1;
}
