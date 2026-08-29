#ifndef SW1000XG_HW_H
#define SW1000XG_HW_H

#include <stddef.h>
#include <stdint.h>

typedef uint32_t (*swxg_read32_fn)(void *context, uint32_t offset);
typedef void (*swxg_write32_fn)(void *context, uint32_t offset, uint32_t value);
typedef void (*swxg_delay_ms_fn)(void *context, uint32_t milliseconds);

typedef struct swxg_io {
    void *context;
    swxg_read32_fn read32;
    swxg_write32_fn write32;
    swxg_delay_ms_fn delay_ms;
} swxg_io;

typedef struct swxg_startup_assets {
    const uint8_t *global_records[5];       /* 18 bytes each */
    const uint32_t *bootstrap_zero_a;       /* 64 words */
    const uint32_t *mpr[11];
    const uint32_t *bootstrap_zero_b;       /* 64 words */
    const uint32_t *cescr;                  /* 6 words */
} swxg_startup_assets;

typedef struct swxg_device {
    swxg_io io;
    uint32_t port1_shadow;
    uint32_t poll_limit;
} swxg_device;

enum {
    SWXG_OK = 0,
    SWXG_INVALID_ARGUMENT = -1,
    SWXG_TIMEOUT = -2
};

enum {
    SWXG_PORT0 = 0x3FF00,
    SWXG_TRPIF = 0x3FF04,
    SWXG_PORT1 = 0x3FF10,
    SWXG_DSP0 = 0x3F000,
    SWXG_DSP_BUSY = 0x80000000u,
    SWXG_PORT1_DIT_DATA = 1u << 27,
    SWXG_PORT1_DIT_LATCH = 1u << 28,
    SWXG_PORT1_DIT_CLOCK = 1u << 29
};

void swxg_init(swxg_device *device, swxg_io io);
void swxg_write_port1(swxg_device *device, uint32_t value);
void swxg_set_port1_bit(swxg_device *device, uint32_t mask, int enabled);
int swxg_dsp_send_words(swxg_device *device, uint16_t dsp_index,
                        uint32_t destination, const uint32_t *words,
                        uint16_t word_count);
int swxg_dsp_send_words_ex(swxg_device *device, uint16_t window_index,
                           uint16_t target_selector, uint32_t destination,
                           const uint32_t *words, uint16_t word_count);
int swxg_dsp_set_word(swxg_device *device, uint16_t dsp_index,
                      uint32_t destination, uint32_t value);
int swxg_dsp_set_word_ex(swxg_device *device, uint16_t window_index,
                         uint16_t target_selector, uint32_t destination,
                         uint32_t value);
int swxg_set_ram(swxg_device *device, uint32_t address, uint32_t value);
void swxg_dit_write(swxg_device *device, uint8_t mode, uint32_t value);
void swxg_write_global_record(swxg_device *device, uint16_t dsp_index,
                              const uint8_t record[18]);
int swxg_startup(swxg_device *device, const swxg_startup_assets *assets);

#endif
