// =====================================================================
//  CraftMiner  --  key-cap icons (see icons.h)
//  Ported from tanmatsu-synthracer-grace, main/icons.c. Changes here are
//  CraftMiner's; synthracer stays the origin to diff against.
// =====================================================================

#include "ui/icons.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_heap_caps.h"
#include "esp_log.h"
#include "pax_codecs.h"

static char const TAG[] = "icons";

#define ICON_W           32
#define ICON_H           32
#define ICON_BUFFER_SIZE (ICON_W * ICON_H * 4)

static char const* icon_filenames[ICON_KEY_COUNT] = {
    [ICON_ESC] = "/int/icons/esc.png",
    [ICON_F1]  = "/int/icons/f1.png",
    [ICON_F2]  = "/int/icons/f2.png",
    [ICON_F3]  = "/int/icons/f3.png",
    [ICON_F4]  = "/int/icons/f4.png",
    [ICON_F5]  = "/int/icons/f5.png",
    [ICON_F6]  = "/int/icons/f6.png",
};

static pax_buf_t s_icons[ICON_KEY_COUNT]  = {0};
static bool      s_loaded[ICON_KEY_COUNT] = {false};
static bool      s_any_missing            = false;

void icons_load(void) {
    for (int i = 0; i < ICON_KEY_COUNT; i++) {
        FILE* fd = fopen(icon_filenames[i], "rb");
        if (!fd) {
            ESP_LOGW(TAG, "icon not found: %s", icon_filenames[i]);
            s_any_missing = true;
            continue;
        }

        void* buffer = heap_caps_calloc(1, ICON_BUFFER_SIZE, MALLOC_CAP_SPIRAM);
        if (!buffer) {
            ESP_LOGE(TAG, "alloc failed for %s", icon_filenames[i]);
            fclose(fd);
            s_any_missing = true;
            continue;
        }

        pax_buf_init(&s_icons[i], buffer, ICON_W, ICON_H, PAX_BUF_32_8888ARGB);

        if (!pax_insert_png_fd(&s_icons[i], fd, 0, 0, 0)) {
            ESP_LOGE(TAG, "decode failed: %s", icon_filenames[i]);
            pax_buf_destroy(&s_icons[i]);
            free(buffer);
            memset(&s_icons[i], 0, sizeof(pax_buf_t));
            s_any_missing = true;
        } else {
            s_loaded[i] = true;
            ESP_LOGI(TAG, "loaded %s", icon_filenames[i]);
        }

        fclose(fd);
    }
}

pax_buf_t* icons_get(icon_key_t key) {
    if (key < 0 || key >= ICON_KEY_COUNT) return NULL;
    if (!s_loaded[key]) return NULL;
    return &s_icons[key];
}

bool icons_any_missing(void) {
    return s_any_missing;
}

int icons_width(icon_key_t key) {
    pax_buf_t* b = icons_get(key);
    return b ? pax_buf_get_width(b) : 0;
}

int icons_height(icon_key_t key) {
    pax_buf_t* b = icons_get(key);
    return b ? pax_buf_get_height(b) : 0;
}

void icons_blit(pax_buf_t* fb, icon_key_t key, float dst_x, float dst_y) {
    pax_buf_t* src = icons_get(key);
    if (!src || !fb) return;
    pax_draw_image(fb, src, dst_x, dst_y);
}
