// =====================================================================
//  CraftMiner  --  what the memory actually costs (see membench.h)
// =====================================================================

#include "game/membench.h"

#include <stdint.h>
#include "esp_heap_caps.h"
#include "esp_log.h"
#include "esp_timer.h"

int esp_clk_cpu_freq(void);

static char const TAG[] = "membench";

#define PX_PSRAM 32768  // 192 KiB of buffers: far past any cache
#define PX_INT    8192  // 48 KiB: internal SRAM has no room for more

// The span loop's pattern, arithmetic removed: depth read, depth write,
// colour write, walking down. `forward` flips the direction so the cost
// of going against the prefetcher can be seen on its own.
//
// `stamp` MUST differ from whatever the last pass wrote, or the depth
// test short-circuits and this measures a read-only loop -- which is
// exactly the mistake the first version of this made, and it under-read
// the cost by a factor of five.
static int64_t pass(uint32_t volatile* depth, uint16_t volatile* colour, int n, bool forward, uint32_t stamp) {
    int const     step = forward ? 1 : -1;
    int           i    = forward ? 0 : n - 1;
    int           hits = 0;
    int64_t const t0   = esp_timer_get_time();
    for (int k = 0; k < n; k++) {
        uint32_t const cell = depth[i];
        if ((cell >> 16) != stamp) {
            depth[i]  = (stamp << 16) | 1u;
            colour[i] = (uint16_t)k;
            hits++;
        }
        i += step;
    }
    int64_t const us = esp_timer_get_time() - t0;
    return hits == n ? us : -us;  // negative: the writes did not happen
}

static void bench(char const* where, uint32_t caps, int n) {
    uint32_t* depth  = heap_caps_malloc((size_t)n * sizeof(uint32_t), caps);
    uint16_t* colour = heap_caps_malloc((size_t)n * sizeof(uint16_t), caps);
    if (depth == NULL || colour == NULL) {
        ESP_LOGW(TAG, "%s: no room (%u KiB wanted)", where, (unsigned)((size_t)n * 6 / 1024));
        heap_caps_free(depth);
        heap_caps_free(colour);
        return;
    }
    uint32_t stamp = 1;
    pass(depth, colour, n, true, stamp++);  // warm: page tables and first fill
    int64_t const fwd = pass(depth, colour, n, true, stamp++);
    int64_t const bwd = pass(depth, colour, n, false, stamp++);
    if (fwd < 0 || bwd < 0) {
        ESP_LOGE(TAG, "%s: the depth test short-circuited; the numbers would be a lie", where);
    } else {
        ESP_LOGI(TAG, "%-8s %6.1f ns/px forward, %6.1f ns/px backward (%.1f / %.1f Mpx/s), %u KiB", where,
                 (double)fwd * 1000.0 / n, (double)bwd * 1000.0 / n, (double)n / (double)fwd, (double)n / (double)bwd,
                 (unsigned)((size_t)n * 6 / 1024));
    }
    heap_caps_free(depth);
    heap_caps_free(colour);
}

// The same pattern with a NARROW depth plane: 16 bits of depth and no
// frame stamp, so 2 B read + 2 B write instead of 4 + 4. That is the
// one structural change available to the engine's fill loops -- it
// costs a per-frame clear of the plane, which is measured here too.
//
// 10 bytes a pixel against 6 is a 40% cut in the traffic, and this says
// what 40% is worth before anything in se_scene.c is touched.
static void bench_narrow(char const* where, uint32_t caps, int n) {
    uint16_t* depth  = heap_caps_malloc((size_t)n * sizeof(uint16_t), caps);
    uint16_t* colour = heap_caps_malloc((size_t)n * sizeof(uint16_t), caps);
    if (depth == NULL || colour == NULL) {
        heap_caps_free(depth);
        heap_caps_free(colour);
        return;
    }
    uint16_t volatile* d = depth;
    uint16_t volatile* c = colour;

    // The clear the stamp exists to avoid.
    int64_t t0 = esp_timer_get_time();
    for (int k = 0; k < n; k++) d[k] = 0;
    int64_t const clear_us = esp_timer_get_time() - t0;

    // Walking backwards, as the real loop does. Depth rises along the
    // span so the test passes every time, which is the expensive case
    // and the one worth quoting.
    t0 = esp_timer_get_time();
    for (int k = n - 1, v = 1; k >= 0; k--, v++) {
        if ((uint16_t)v > d[k]) {
            d[k] = (uint16_t)v;
            c[k] = (uint16_t)v;
        }
    }
    int64_t const us = esp_timer_get_time() - t0;

    ESP_LOGI(TAG, "%-8s %6.1f ns/px with a 16-bit depth plane (+%.2f ms to clear %u KiB a frame)", where,
             (double)us * 1000.0 / n, (double)clear_us / 1000.0, (unsigned)((size_t)n * 2 / 1024));
    heap_caps_free(depth);
    heap_caps_free(colour);
}

void membench_run(void) {
    // Without this every "ns per pixel" below is uninterpretable: the
    // same number is a tight loop at 360 MHz and a lazy one at 160.
    int const hz = esp_clk_cpu_freq();
    ESP_LOGI(TAG, "CPU %d MHz -- 1 ns is %.2f cycles", hz / 1000000, (double)hz / 1e9);

    // A pure-arithmetic control: the fill loop's float work with no
    // memory in it at all. Whatever this costs, no amount of cache or
    // SIMD-free tuning of the memory pattern can go below it.
    {
        float         d = 0.0f, acc = 0.0f;
        int const     n = 200000;
        int64_t const t0 = esp_timer_get_time();
        for (int k = 0; k < n; k++) {
            d += 1.031f;
            int di = (int)d;          // the float->int the depth test needs
            if (di < 0) di = 0;
            acc += (float)(di & 0xFFFF);
        }
        int64_t const us = esp_timer_get_time() - t0;
        ESP_LOGI(TAG, "arithmetic only (add + float->int + compare): %.1f ns/px (%.1f cycles), acc %.0f",
                 (double)us * 1000.0 / n, (double)us * 1e-6 * (double)hz / n, (double)acc);
    }

    ESP_LOGI(TAG, "span-loop memory pattern, no arithmetic: 4 B depth read + 4 B write + 2 B colour write");
    bench("PSRAM", MALLOC_CAP_SPIRAM, PX_PSRAM);
    bench("internal", MALLOC_CAP_INTERNAL, PX_INT);
    bench_narrow("PSRAM", MALLOC_CAP_SPIRAM, PX_PSRAM);
    bench_narrow("internal", MALLOC_CAP_INTERNAL, PX_INT);
}
