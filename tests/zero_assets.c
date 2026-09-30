/* Zero-filled startup assets for host tests: exercises the full sequence
 * without any Yamaha-derived data. */
#include "../src/kmdf/sw1000xg_assets.generated.h"

static const uint8_t global[18];
static const uint32_t words[0x140];

static const swxg_startup_assets assets = {
    {global, global, global, global, global},
    {words, words, words, words, words, words, words, words, words, words,
     words},
    words
};

const swxg_startup_assets *SwxgGetStartupAssets(void)
{
    return &assets;
}
