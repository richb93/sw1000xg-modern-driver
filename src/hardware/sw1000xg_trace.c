#include "sw1000xg_trace.h"

static void record(swxg_trace *trace, uint32_t kind, uint32_t offset,
                   uint32_t value)
{
    swxg_trace_entry *last;
    if (trace->count != 0) {
        last = &trace->entries[trace->count - 1];
        if ((kind == SWXG_TRACE_READ || kind == SWXG_TRACE_READ8) &&
            last->kind == kind &&
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

static uint8_t trace_read8(void *context, uint32_t offset)
{
    swxg_trace *trace = context;
    uint8_t value = trace->inner.read8(trace->inner.context, offset);
    record(trace, SWXG_TRACE_READ8, offset, value);
    return value;
}

static void trace_write8(void *context, uint32_t offset, uint8_t value)
{
    swxg_trace *trace = context;
    record(trace, SWXG_TRACE_WRITE8, offset, value);
    trace->inner.write8(trace->inner.context, offset, value);
}

static void trace_delay_us(void *context, uint32_t microseconds)
{
    swxg_trace *trace = context;
    record(trace, SWXG_TRACE_DELAY_US, 0, microseconds);
    trace->inner.delay_us(trace->inner.context, microseconds);
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
    io.read8 = trace->inner.read8 ? trace_read8 : 0;
    io.write8 = trace->inner.write8 ? trace_write8 : 0;
    io.delay_us = trace->inner.delay_us ? trace_delay_us : 0;
    return io;
}
