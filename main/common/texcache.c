// =====================================================================
//  Showreel asset  --  shared texture cache (see texcache.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/common/texcache.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "common/texcache.h"
#include <stdbool.h>
#include <stdio.h>
#include <string.h>
#include "esp_log.h"

static char const TAG[] = "texcache";

#define TEXCACHE_MAX   48
// Textures stay in PSRAM for now (see devdocs/performance.md: internal
// SRAM would save ~4% of the textured pass).
#define TEXCACHE_FLAGS 0

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
