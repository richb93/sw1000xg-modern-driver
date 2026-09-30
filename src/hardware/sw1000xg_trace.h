#ifndef SW1000XG_TRACE_H
#define SW1000XG_TRACE_H

#include "sw1000xg_hw.h"

/*
 * Optional MMIO trace: wraps an swxg_io, forwards every access and records it
 * in a caller-supplied array. Consecutive identical reads (busy polls) are
 * folded into one entry with a repeat count. No allocation, no locking.
 *
 * Text form, one entry per line, parsed by tools/recipe_trace.py:
 *   SWXG W <offset> <value>
 *   SWXG R <offset> <value> <repeat>
 *   SWXG D <milliseconds>
 *   SWXG END <result> <dropped>
 * Offsets and values are hexadecimal without a prefix.
 */

typedef enum swxg_trace_kind {
    SWXG_TRACE_WRITE = 1,
    SWXG_TRACE_READ = 2,
    SWXG_TRACE_DELAY = 3
} swxg_trace_kind;

typedef struct swxg_trace_entry {
    uint32_t kind;
    uint32_t offset;
    uint32_t value;
    uint32_t repeat;
} swxg_trace_entry;

typedef struct swxg_trace {
    swxg_io inner;
    swxg_trace_entry *entries;
    size_t capacity;
    size_t count;
    size_t dropped;
} swxg_trace;

void swxg_trace_init(swxg_trace *trace, swxg_io inner,
                     swxg_trace_entry *entries, size_t capacity);
/* Returns an io whose context is the trace; keep the trace alive while used. */
swxg_io swxg_trace_io(swxg_trace *trace);

#endif
