// =====================================================================
//  CraftMiner  --  the game's own settings (see settings.h)
// =====================================================================

#include "ui/settings.h"

#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "game/input.h"
#include "i18n/i18n.h"
#include "se_audio.h"
#include "se_bindings.h"
#include "world/vfs_compat.h"

static char const TAG[] = "settings";

static char s_path[192];
static char s_tmp[192];

static int  s_view     = SETTINGS_VIEW_DEFAULT;
static bool s_textured = true;
static bool s_half     = true;
static bool s_music    = true;
static bool s_sfx      = true;
static uint8_t s_music_vol = 100;  // per-class mix levels; the device volume is the badge's
static uint8_t s_sfx_vol   = 100;
static bool s_gyro     = false;
static bool s_clouds   = true;
static bool s_third    = false;
static bool s_left     = false;

// Bindings are keyed by the action's stable short name (input.c's
// table, which never changes once shipped), so reordering the actions
// never moves anyone's keys.
#define KEY_PREFIX "key."

// --- The file ---------------------------------------------------------------

static void apply_line(char* line) {
    char* eq = strchr(line, '=');
    if (eq == NULL || line[0] == '#') return;
    *eq               = '\0';
    char const* key   = line;
    char const* value = eq + 1;
    unsigned long const v = strtoul(value, NULL, 0);

    if (strcmp(key, "language") == 0) {
        // The only setting whose value is a word: "de", "nl-BE". One
        // this build does not know leaves the language alone, which is
        // English unless something else has already set it.
        cm_lang_t lang;
        if (i18n_language_from_code(value, &lang)) i18n_set_language(lang);
    } else if (strcmp(key, "view") == 0) {
        s_view = v < SETTINGS_VIEW_COUNT ? (int)v : SETTINGS_VIEW_DEFAULT;
    } else if (strcmp(key, "textures") == 0) {
        s_textured = v != 0;
    } else if (strcmp(key, "half_res") == 0) {
        s_half = v != 0;
    } else if (strcmp(key, "music") == 0) {
        s_music = v != 0;
    } else if (strcmp(key, "effects") == 0) {
        s_sfx = v != 0;
    } else if (strcmp(key, "music_volume") == 0) {
        s_music_vol = v > 100 ? 100 : (uint8_t)v;
    } else if (strcmp(key, "effects_volume") == 0) {
        s_sfx_vol = v > 100 ? 100 : (uint8_t)v;
    } else if (strcmp(key, "gyro") == 0) {
        s_gyro = v != 0;
    } else if (strcmp(key, "clouds") == 0) {
        s_clouds = v != 0;
    } else if (strcmp(key, "third_person") == 0) {
        s_third = v != 0;
    } else if (strcmp(key, "left_handed") == 0) {
        s_left = v != 0;
    } else if (strncmp(key, KEY_PREFIX, strlen(KEY_PREFIX)) == 0) {
        char const* name = key + strlen(KEY_PREFIX);
        for (int a = 0; a < CM_ACTION_COUNT; a++) {
            if (strcmp(name, input_action_id((cm_action_t)a)) == 0 && v != 0 && v <= 0xFFFFu) {
                se_bindings_set(a, (uint16_t)v);
            }
        }
    }
    // Anything else is from a newer build, or a typo: ignored.
}

// The whole file in one read: it is a few hundred bytes, and the stdio
// the graceloader exports has fread but no fgets.
static bool read_file(char const* path) {
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;
    static char buf[2048];
    size_t const n = fread(buf, 1, sizeof(buf) - 1, f);
    fclose(f);
    buf[n] = '\0';
    char* line = buf;
    while (*line != '\0') {
        char* end = line + strcspn(line, "\r\n");
        char const was = *end;
        *end = '\0';
        apply_line(line);
        line = was == '\0' ? end : end + 1;
    }
    return true;
}

void settings_save(void) {
    if (s_path[0] == '\0') return;
    FILE* f = fopen(s_tmp, "wb");
    if (f == NULL) {
        ESP_LOGW(TAG, "could not write %s", s_tmp);
        return;
    }
    fputs("# CraftMiner settings. Volume and brightness are the badge's own and live\n"
          "# with the launcher. Keys are BSP scancodes; delete a line to get its default.\n", f);
    fprintf(f, "language=%s\n", i18n_language_code(i18n_language()));
    fprintf(f,
            "view=%d\ntextures=%d\nhalf_res=%d\nclouds=%d\nthird_person=%d\nleft_handed=%d\nmusic=%d\neffects=%d\n"
            "music_volume=%u\neffects_volume=%u\ngyro=%d\n",
            s_view, s_textured, s_half, s_clouds, s_third, s_left, s_music, s_sfx, (unsigned)s_music_vol,
            (unsigned)s_sfx_vol, s_gyro);
    for (int a = 0; a < CM_ACTION_COUNT; a++) {
        fprintf(f, KEY_PREFIX "%s=0x%04x\n", input_action_id((cm_action_t)a), (unsigned)input_key((cm_action_t)a));
    }
    bool const ok = fflush(f) == 0;
    fclose(f);
    if (!ok) {
        ESP_LOGW(TAG, "writing %s failed", s_tmp);
        return;
    }
    // FAT will not rename over an existing file. Between the remove and
    // the rename only the .tmp exists, which settings_load also reads.
    cm_remove(s_path);
    if (!cm_rename(s_tmp, s_path)) ESP_LOGW(TAG, "could not move %s into place", s_tmp);
}

void settings_load(char const* dir) {
    snprintf(s_path, sizeof(s_path), "%s/settings.txt", dir);
    snprintf(s_tmp, sizeof(s_tmp), "%s/settings.tmp", dir);

    char const* from = s_path;
    if (!read_file(s_path)) {
        from = read_file(s_tmp) ? s_tmp : NULL;
    }
    // A first run has no file: the defaults stand, and nothing is
    // written until something is changed.
    audio_mixer_set_music_enabled(s_music);
    audio_mixer_set_group_enabled(SETTINGS_SFX_GROUP, s_sfx);
    audio_mixer_set_music_volume(s_music_vol);
    audio_mixer_set_group_volume(SETTINGS_SFX_GROUP, s_sfx_vol);
    ESP_LOGI(TAG,
             "%s: language %s, view %d, textures %s, %s resolution, music %s (%u%%), effects %s (%u%%), "
             "gyroscope %s",
             from ? from : "no settings file, defaults", i18n_language_code(i18n_language()), s_view,
             s_textured ? "on" : "off", s_half ? "half" : "full", s_music ? "on" : "off", (unsigned)s_music_vol,
             s_sfx ? "on" : "off", (unsigned)s_sfx_vol, s_gyro ? "on" : "off");
}

// --- The values -------------------------------------------------------------

int settings_view(void) {
    return s_view;
}

void settings_set_view(int view) {
    if (view < 0 || view >= SETTINGS_VIEW_COUNT || view == s_view) return;
    s_view = view;
    settings_save();
}

bool settings_textured(void) {
    return s_textured;
}

void settings_set_textured(bool on) {
    if (on == s_textured) return;
    s_textured = on;
    settings_save();
}

bool settings_half_res(void) {
    return s_half;
}

void settings_set_half_res(bool on) {
    if (on == s_half) return;
    s_half = on;
    settings_save();
}

bool settings_music(void) {
    return s_music;
}

void settings_set_music(bool on) {
    if (on == s_music) return;
    s_music = on;
    settings_save();
    audio_mixer_set_music_enabled(on);
}

bool settings_sfx(void) {
    return s_sfx;
}

uint8_t settings_music_volume(void) {
    return s_music_vol;
}

void settings_set_music_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    if (pct == s_music_vol) return;
    s_music_vol = pct;
    settings_save();
    audio_mixer_set_music_volume(pct);
}

uint8_t settings_sfx_volume(void) {
    return s_sfx_vol;
}

void settings_set_sfx_volume(uint8_t pct) {
    if (pct > 100) pct = 100;
    if (pct == s_sfx_vol) return;
    s_sfx_vol = pct;
    settings_save();
    audio_mixer_set_group_volume(SETTINGS_SFX_GROUP, pct);
}

void settings_set_sfx(bool on) {
    if (on == s_sfx) return;
    s_sfx = on;
    settings_save();
    audio_mixer_set_group_enabled(SETTINGS_SFX_GROUP, on);
}

bool settings_gyro(void) {
    return s_gyro;
}

void settings_set_gyro(bool on) {
    if (on == s_gyro) return;
    s_gyro = on;
    settings_save();
}

bool settings_clouds(void) {
    return s_clouds;
}

void settings_set_clouds(bool on) {
    if (on == s_clouds) return;
    s_clouds = on;
    settings_save();
}

bool settings_third_person(void) {
    return s_third;
}

void settings_set_third_person(bool on) {
    if (on == s_third) return;
    s_third = on;
    settings_save();
}

bool settings_left_handed(void) {
    return s_left;
}

void settings_set_left_handed(bool on) {
    if (on == s_left) return;
    s_left = on;
    settings_save();
}
