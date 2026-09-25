// =====================================================================
//  Showreel asset  --  shared texture cache (see texcache.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/common/texcache.c. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "common/texcache.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static char const TAG[] = "texcache";

#define TEXCACHE_MAX 48

// INTERNAL SRAM, not PSRAM (the showreel's default, and the reason its
// note said "would save ~4%" rather than "saves").
//
// The rasteriser reads one texel per pixel it draws, and those reads
// land wherever the triangle happens to map -- not in the neat runs the
// framebuffer gets -- so they are exactly the access pattern a cache
// handles worst. SynthMiner's textures are 16x16 RGB565: **512 bytes
// each, about 9 KiB for all eighteen**, against 160 KiB of internal
// SRAM free after boot. It is the cheapest thing in the program to put
// somewhere fast.
//
// The engine falls back to PSRAM per texture if internal will not hold
// it, and reports which it used in `se_texture_t.internal`, so this can
// never fail a load -- it can only quietly stop helping. texcache_init
// logs the split for that reason.
#define TEXCACHE_FLAGS SE_TEXTURE_INTERNAL

typedef struct {
    char          file[32];
    se_texture_t* tex;
    bool          failed;
} entry_t;

static entry_t s_entries[TEXCACHE_MAX];
static int     s_n;
static char    s_dir[160] = ".";

void texcache_init(char const* asset_dir) {
    snprintf(s_dir, sizeof(s_dir), "%s", asset_dir ? asset_dir : ".");
}

void texcache_shutdown(void) {
    for (int i = 0; i < s_n; i++) se_texture_unload(s_entries[i].tex);
    memset(s_entries, 0, sizeof(s_entries));
    s_n = 0;
}

// Where the texels actually ended up, and how much they took. A texture
// that silently fell back to PSRAM is the failure mode worth seeing.
void texcache_report(void) {
    int    internal = 0, psram = 0;
    size_t internal_bytes = 0;
    for (int i = 0; i < s_n; i++) {
        se_texture_t const* t = s_entries[i].tex;
        if (t == NULL) continue;
        size_t const bytes = (size_t)t->w * (size_t)t->h * sizeof(uint16_t);
        if (t->internal) {
            internal++;
            internal_bytes += bytes;
        } else {
            psram++;
        }
    }
    ESP_LOGI(TAG, "%d textures: %d in internal SRAM (%u B), %d in PSRAM", s_n, internal, (unsigned)internal_bytes,
             psram);
}

se_texture_t const* texcache_get(char const* file) {
    for (int i = 0; i < s_n; i++) {
        if (strcmp(s_entries[i].file, file) == 0) return s_entries[i].tex;
    }
    if (s_n == TEXCACHE_MAX) {
        ESP_LOGE(TAG, "cache full, %s not loaded", file);
        return NULL;
    }
    entry_t* e = &s_entries[s_n++];
    strlcpy(e->file, file, sizeof(e->file));
    char path[224];
    snprintf(path, sizeof(path), "%s/%s", s_dir, file);
    e->tex    = se_texture_load(path, TEXCACHE_FLAGS);
    e->failed = (e->tex == NULL);
    if (e->failed) ESP_LOGW(TAG, "%s missing -- drawn flat", file);
    return e->tex;
}
