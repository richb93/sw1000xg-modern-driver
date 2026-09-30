#include "sw1000xg_trace.h"

static void record(swxg_trace *trace, uint32_t kind, uint32_t offset,
                   uint32_t value)
{
    swxg_trace_entry *last;
    if (trace->count != 0) {
        last = &trace->entries[trace->count - 1];
        if (kind == SWXG_TRACE_READ && last->kind == SWXG_TRACE_READ &&
            last->offset == offset && last->value == value &&
            last->repeat != 0xFFFFFFFFu) {
            ++last->repeat;
            return;
        }
    }
    if (trace->count == trace->capacity) {
        ++trace->dropped;
        return;
    }
    last = &trace->entries[trace->count++];
    last->kind = kind;
    last->offset = offset;
    last->value = value;
    last->repeat = 1;
}

static uint32_t trace_read32(void *context, uint32_t offset)
{
    swxg_trace *trace = context;
    uint32_t value = trace->inner.read32(trace->inner.context, offset);
    record(trace, SWXG_TRACE_READ, offset, value);
    return value;
}

static void trace_write32(void *context, uint32_t offset, uint32_t value)
{
    swxg_trace *trace = context;
    record(trace, SWXG_TRACE_WRITE, offset, value);
    trace->inner.write32(trace->inner.context, offset, value);
}

static void trace_delay_ms(void *context, uint32_t milliseconds)
{
    swxg_trace *trace = context;
    record(trace, SWXG_TRACE_DELAY, 0, milliseconds);
    trace->inner.delay_ms(trace->inner.context, milliseconds);
}

void swxg_trace_init(swxg_trace *trace, swxg_io inner,
                     swxg_trace_entry *entries, size_t capacity)
{
    trace->inner = inner;
    trace->entries = entries;
    trace->capacity = capacity;
    trace->count = 0;
    trace->dropped = 0;
}

swxg_io swxg_trace_io(swxg_trace *trace)
{
    swxg_io io;
    io.context = trace;
    io.read32 = trace_read32;
    io.write32 = trace_write32;
    io.delay_ms = trace_delay_ms;
    return io;
}
