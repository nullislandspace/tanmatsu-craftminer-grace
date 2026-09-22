// =====================================================================
//  CraftMiner  --  the menus (see menu.h)
// =====================================================================

#include "ui/menu.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "common/rng.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "game/input.h"
#include "se_direct565.h"
#include "se_hw.h"
#include "se_text.h"
#include "se_ui.h"
#include "ui/keybind_ui.h"
#include "ui/settings.h"

static char const TAG[] = "menu";

typedef enum {
    SCR_NONE = 0,
    SCR_TITLE,     // the bar under the title
    SCR_WORLDS,    // the save slots
    SCR_WORLD,     // one world: play, rename, delete
    SCR_NEW,       // a new world: name, seed, create
    SCR_TEXT,      // typing a name or a seed
    SCR_DELETE,    // are you sure?
    SCR_SETTINGS,
    SCR_CONTROLS,
    SCR_GRAPHICS,
    SCR_AUDIO,
    SCR_DISPLAY,
    SCR_PAUSE,
    SCR_COUNT
} screen_t;

// One action per key per frame, whichever of its events arrived (see
// menu.h). A bitmask, so a scancode and a navigation event for the same
// press collapse into one.
enum {
    ACT_UP    = 1u << 0,
    ACT_DOWN  = 1u << 1,
    ACT_LEFT  = 1u << 2,
    ACT_RIGHT = 1u << 3,
    ACT_OK    = 1u << 4,
    ACT_BACK  = 1u << 5,
    ACT_BKSP  = 1u << 6,
};

static screen_t s_scr;
static int      s_cursor[SCR_COUNT];  // remembered per screen, so Back lands where you were
static screen_t s_settings_parent;    // the title or the pause menu

static uint32_t s_act;
static char     s_typed[16];
static int      s_typed_n;

// The save slots, read when the slot list opens. Reading is a file
// open per slot, which is fine a few times a session and not fine per
// frame.
static world_meta_t s_slot_meta[CM_SLOTS];
static bool         s_slot_used[CM_SLOTS];
static int          s_slot;  // the slot the WORLD / NEW / DELETE screens are about

// The new-world form.
static char s_new_name[CM_WORLD_NAME_MAX];
static char s_new_seed[12];

// Typing. The text is edited in a copy and only written back on Enter,
// so Esc really does leave the old value alone.
typedef enum { FIELD_NEW_NAME = 0, FIELD_NEW_SEED, FIELD_RENAME } field_t;
static field_t     s_field;
static char        s_text[CM_WORLD_NAME_MAX];
static int         s_text_cap;
static screen_t    s_text_return;
static char const* s_text_title;

// Set when a menu opens from inside an input event: the rest of that
// batch belongs to the key that opened it. Esc arrives as a scancode AND
// a navigation event, and without this the second would close the pause
// menu the first had just opened.
static bool s_swallow;

static char    s_status[48];
static int64_t s_status_until;

// --- Small things ---------------------------------------------------------

void menu_status(char const* msg) {
    snprintf(s_status, sizeof(s_status), "%s", msg ? msg : "");
    s_status_until = esp_timer_get_time() + 2500000;
}

static char const* status_line(void) {
    return (s_status[0] != '\0' && esp_timer_get_time() < s_status_until) ? s_status : NULL;
}

static void go(screen_t scr) {
    s_scr = scr;
}

static void refresh_slots(void) {
    for (int i = 0; i < CM_SLOTS; i++) {
        s_slot_used[i] = worldstore_slot_peek(i, &s_slot_meta[i]);
    }
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// Up and down over `n` rows. No wrap, like the engine's own menus.
static void nav(int* cursor, int n) {
    if (s_act & ACT_UP) (*cursor)--;
    if (s_act & ACT_DOWN) (*cursor)++;
    *cursor = clampi(*cursor, 0, n - 1);
}

static uint8_t pct_step(uint8_t cur, int delta) {
    return (uint8_t)clampi((int)cur + delta, 0, 100);
}

// A seed from what was typed: a number is that number, anything else
// is hashed (so "Kurt" is a seed, the way it is in Minecraft), and
// nothing at all is random.
static uint32_t seed_from(char const* t) {
    if (t[0] == '\0') {
        return (uint32_t)cm_mix64((uint64_t)esp_timer_get_time() ^ 0x9E3779B97F4A7C15ull);
    }
    char const* p   = t + (t[0] == '-' ? 1 : 0);
    bool        num = *p != '\0';
    for (char const* c = p; *c; c++) num = num && *c >= '0' && *c <= '9';
    if (num) {
        // By hand: strtoll is not exported by the graceloader. Wraps
        // modulo 2^32 like any seed, and a minus sign negates it.
        uint32_t v = 0;
        for (char const* c = p; *c; c++) v = v * 10u + (uint32_t)(*c - '0');
        return t[0] == '-' ? (uint32_t)(0u - v) : v;
    }
    uint32_t h = 2166136261u;  // FNV-1a
    for (char const* c = t; *c; c++) h = (h ^ (uint8_t)*c) * 16777619u;
    return h;
}

static void begin_text(field_t field, char const* title, char const* initial, int cap, screen_t back) {
    s_field       = field;
    s_text_title  = title;
    s_text_cap    = cap < (int)sizeof(s_text) ? cap : (int)sizeof(s_text);
    s_text_return = back;
    snprintf(s_text, (size_t)s_text_cap, "%s", initial);
    s_typed_n = 0;
    go(SCR_TEXT);
}

// --- Opening and closing ----------------------------------------------------

void menu_open_title(void) {
    s_act     = 0;
    s_cursor[SCR_TITLE] = 0;
    go(SCR_TITLE);
}

void menu_open_pause(void) {
    s_act               = 0;
    s_swallow           = true;
    s_cursor[SCR_PAUSE] = 0;
    go(SCR_PAUSE);
}

void menu_close(void) {
    go(SCR_NONE);
}

bool menu_active(void) {
    return s_scr != SCR_NONE;
}

bool menu_show(char const* name) {
    static struct {
        char const* name;
        screen_t    scr;
    } const SHOW[] = {
        {"worlds", SCR_WORLDS},       {"world", SCR_WORLD},       {"newworld", SCR_NEW},
        {"settings", SCR_SETTINGS},   {"controls", SCR_CONTROLS}, {"graphics", SCR_GRAPHICS},
        {"audio", SCR_AUDIO},         {"display", SCR_DISPLAY},   {"pause", SCR_PAUSE},
    };
    for (size_t i = 0; i < sizeof(SHOW) / sizeof(SHOW[0]); i++) {
        if (strcmp(name, SHOW[i].name) != 0) continue;
        refresh_slots();
        s_slot = 0;
        snprintf(s_new_name, sizeof(s_new_name), "World 2");
        s_new_seed[0]     = '\0';
        s_settings_parent = SCR_TITLE;
        go(SHOW[i].scr);
        return true;
    }
    return false;
}

bool menu_in_panel(void) {
    return s_scr != SCR_NONE && s_scr != SCR_TITLE;
}

// --- Input -----------------------------------------------------------------

void menu_event(bsp_input_event_t const* ev) {
    if (s_scr == SCR_NONE || s_swallow) return;

    if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if ((sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return;
        switch (sc) {
            case BSP_INPUT_SCANCODE_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_SCANCODE_ENTER:
            case BSP_INPUT_SCANCODE_ESCAPED_KPENTER: s_act |= ACT_OK; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT: s_act |= ACT_RIGHT; break;
            case BSP_INPUT_SCANCODE_BACKSPACE: s_act |= ACT_BKSP; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: s_act |= ACT_OK; break;
            case BSP_INPUT_NAVIGATION_KEY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_NAVIGATION_KEY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_NAVIGATION_KEY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_NAVIGATION_KEY_RIGHT: s_act |= ACT_RIGHT; break;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: s_act |= ACT_BKSP; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_KEYBOARD && s_scr == SCR_TEXT) {
        // Printable characters only: Enter, Backspace and Esc also send
        // a character on some keyboards, and they are actions here.
        char const c = ev->args_keyboard.ascii;
        if (c >= 32 && c < 127 && s_typed_n < (int)sizeof(s_typed)) s_typed[s_typed_n++] = c;
    }
}

// --- The screens: what a key does -----------------------------------------

#define TITLE_ROWS 3

static menu_cmd_t update_title(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_TITLE];
    // A bar, so left and right move along it; up and down too, because
    // everything else is a list and a thumb does not know the difference.
    if (s_act & (ACT_LEFT | ACT_UP)) (*cur)--;
    if (s_act & (ACT_RIGHT | ACT_DOWN)) (*cur)++;
    *cur = clampi(*cur, 0, TITLE_ROWS - 1);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0:
                refresh_slots();
                go(SCR_WORLDS);
                break;
            case 1:
                s_settings_parent = SCR_TITLE;
                go(SCR_SETTINGS);
                break;
            default: cmd.kind = MENU_CMD_LEAVE; break;
        }
    } else if (s_act & ACT_BACK) {
        // Esc on the title leaves, as it always has: there is nothing
        // unsaved on the title to lose.
        cmd.kind = MENU_CMD_LEAVE;
    }
    return cmd;
}

static menu_cmd_t update_worlds(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_WORLDS];
    nav(cur, CM_SLOTS + 1);
    if (s_act & ACT_OK) {
        if (*cur == CM_SLOTS) {
            go(SCR_TITLE);
        } else if (s_slot_used[*cur]) {
            s_slot               = *cur;
            s_cursor[SCR_WORLD] = 0;
            go(SCR_WORLD);
        } else {
            s_slot = *cur;
            snprintf(s_new_name, sizeof(s_new_name), "World %d", *cur + 1);
            s_new_seed[0]      = '\0';
            s_cursor[SCR_NEW] = 0;
            go(SCR_NEW);
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_TITLE);
    }
    return cmd;
}

static menu_cmd_t update_world(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_WORLD];
    nav(cur, 4);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0:
                cmd.kind = MENU_CMD_PLAY;
                cmd.slot = s_slot;
                break;
            case 1:
                begin_text(FIELD_RENAME, "Rename world", s_slot_meta[s_slot].name, CM_WORLD_NAME_MAX, SCR_WORLD);
                break;
            case 2:
                // "No" first, so a second Enter does not delete a world.
                s_cursor[SCR_DELETE] = 0;
                go(SCR_DELETE);
                break;
            default: go(SCR_WORLDS); break;
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_WORLDS);
    }
    return cmd;
}

static menu_cmd_t update_new(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_NEW];
    nav(cur, 4);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0: begin_text(FIELD_NEW_NAME, "World name", s_new_name, CM_WORLD_NAME_MAX, SCR_NEW); break;
            case 1:
                begin_text(FIELD_NEW_SEED, "Seed (empty for random)", s_new_seed, (int)sizeof(s_new_seed), SCR_NEW);
                break;
            case 2:
                if (s_new_name[0] == '\0') {
                    menu_status("A world needs a name");
                    break;
                }
                cmd.kind = MENU_CMD_CREATE;
                cmd.slot = s_slot;
                cmd.seed = seed_from(s_new_seed);
                snprintf(cmd.name, sizeof(cmd.name), "%s", s_new_name);
                break;
            default: go(SCR_WORLDS); break;
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_WORLDS);
    }
    return cmd;
}

static void update_text(void) {
    int len = (int)strlen(s_text);
    for (int i = 0; i < s_typed_n && len + 1 < s_text_cap; i++) {
        s_text[len++] = s_typed[i];
        s_text[len]   = '\0';
    }
    s_typed_n = 0;
    if ((s_act & ACT_BKSP) && len > 0) s_text[--len] = '\0';

    if (s_act & ACT_OK) {
        switch (s_field) {
            case FIELD_NEW_NAME: snprintf(s_new_name, sizeof(s_new_name), "%s", s_text); break;
            case FIELD_NEW_SEED: snprintf(s_new_seed, sizeof(s_new_seed), "%s", s_text); break;
            case FIELD_RENAME:
                if (s_text[0] == '\0') {
                    menu_status("A world needs a name");
                    return;
                }
                if (worldstore_rename(s_slot_meta[s_slot].slug, s_text)) {
                    refresh_slots();
                    menu_status("Renamed");
                } else {
                    menu_status("Could not rename it");
                }
                break;
        }
        go(s_text_return);
    } else if (s_act & ACT_BACK) {
        go(s_text_return);
    }
}

static void update_delete(void) {
    int* cur = &s_cursor[SCR_DELETE];
    nav(cur, 2);
    if (s_act & ACT_OK) {
        if (*cur == 1) {
            bool const ok = worldstore_delete(s_slot_meta[s_slot].slug);
            ESP_LOGI(TAG, "deleted slot %d (\"%s\"): %s", s_slot + 1, s_slot_meta[s_slot].name, ok ? "ok" : "FAILED");
            menu_status(ok ? "World deleted" : "Could not delete it all");
            refresh_slots();
            go(SCR_WORLDS);
        } else {
            go(SCR_WORLD);
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_WORLD);
    }
}

#define SETTINGS_ROWS 5

static void update_settings(void) {
    int* cur = &s_cursor[SCR_SETTINGS];
    nav(cur, SETTINGS_ROWS);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0: go(SCR_CONTROLS); break;
            case 1: go(SCR_GRAPHICS); break;
            case 2: go(SCR_AUDIO); break;
            case 3: go(SCR_DISPLAY); break;
            default: go(s_settings_parent); break;
        }
    } else if (s_act & ACT_BACK) {
        go(s_settings_parent);
    }
}

// Rows: the gyroscope checkbox (first, as in synthracer), every action,
// then "Reset to defaults", then "Back".
#define CONTROLS_FIRST_KEY 1
#define CONTROLS_ROWS      (CONTROLS_FIRST_KEY + CM_ACTION_COUNT + 2)

static void update_controls(void) {
    int* cur = &s_cursor[SCR_CONTROLS];
    nav(cur, CONTROLS_ROWS);
    if ((s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) && *cur == 0) {
        settings_set_gyro(!settings_gyro());
    } else if (s_act & ACT_OK) {
        int const key = *cur - CONTROLS_FIRST_KEY;
        if (key < CM_ACTION_COUNT) {
            // The engine's blocking "press a key" capture, as synthracer
            // uses it. It takes any key, Esc and the cursor keys included
            // (the cursor keys since engine 2.1), so there is no cancel:
            // pressing the key it already had keeps it.
            cm_action_t const a  = (cm_action_t)key;
            uint16_t const    sc = se_ui_capture_key(input_action_label(a));
            if (sc != 0) {
                char name[24];
                input_bind(a, sc);
                ESP_LOGI(TAG, "%s bound to %s", input_action_label(a), input_key_name(sc, name, sizeof(name)));
            }
        } else if (key == CM_ACTION_COUNT) {
            input_reset_defaults();
            menu_status("Every key is back to its default");
        } else {
            go(SCR_SETTINGS);
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_SETTINGS);
    }
}

static char const* const VIEW_NAMES[SETTINGS_VIEW_COUNT] = {"Near", "Medium", "Far"};
#define GRAPHICS_ROWS 7

static menu_cmd_t update_graphics(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_GRAPHICS];
    nav(cur, GRAPHICS_ROWS);
    int const dir = (s_act & ACT_RIGHT) ? 1 : (s_act & ACT_LEFT) ? -1 : (s_act & ACT_OK) ? 1 : 0;
    if (dir != 0) {
        switch (*cur) {
            case 0: {
                // Enter walks round; left and right stop at the ends.
                int v = settings_view() + dir;
                if (s_act & ACT_OK) v = (v + SETTINGS_VIEW_COUNT) % SETTINGS_VIEW_COUNT;
                settings_set_view(clampi(v, 0, SETTINGS_VIEW_COUNT - 1));
                cmd.kind = MENU_CMD_GRAPHICS;
            } break;
            case 1:
                settings_set_textured(!settings_textured());
                cmd.kind = MENU_CMD_GRAPHICS;
                break;
            case 2:
                settings_set_half_res(!settings_half_res());
                cmd.kind = MENU_CMD_GRAPHICS;
                break;
            case 3: settings_set_clouds(!settings_clouds()); break;
            case 4: settings_set_third_person(!settings_third_person()); break;
            case 5: settings_set_left_handed(!settings_left_handed()); break;
            default:
                if (s_act & ACT_OK) go(SCR_SETTINGS);
                break;
        }
    }
    if (s_act & ACT_BACK) go(SCR_SETTINGS);
    return cmd;
}

static void update_audio(void) {
    int* cur = &s_cursor[SCR_AUDIO];
    nav(cur, 4);
    int const step = (s_act & ACT_RIGHT) ? SE_HW_VOLUME_STEP_PCT : (s_act & ACT_LEFT) ? -SE_HW_VOLUME_STEP_PCT : 0;
    switch (*cur) {
        case 0:
            if (step != 0) se_hw_set_volume(pct_step(se_hw_get_volume(), step));
            break;
        case 1:
            if (s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) settings_set_music(!settings_music());
            break;
        case 2:
            if (s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) settings_set_sfx(!settings_sfx());
            break;
        default:
            if (s_act & ACT_OK) go(SCR_SETTINGS);
            break;
    }
    if (s_act & ACT_BACK) go(SCR_SETTINGS);
}

static void update_display(void) {
    int* cur = &s_cursor[SCR_DISPLAY];
    nav(cur, 4);
    int const step =
        (s_act & ACT_RIGHT) ? SE_HW_BRIGHTNESS_STEP_PCT : (s_act & ACT_LEFT) ? -SE_HW_BRIGHTNESS_STEP_PCT : 0;
    if (step != 0) {
        switch (*cur) {
            case 0: se_hw_set_display_brightness(pct_step(se_hw_get_display_brightness(), step)); break;
            case 1: se_hw_set_keyboard_brightness(pct_step(se_hw_get_keyboard_brightness(), step)); break;
            case 2: se_hw_set_led_brightness(pct_step(se_hw_get_led_brightness(), step)); break;
            default: break;
        }
    }
    if ((s_act & ACT_OK) && *cur == 3) go(SCR_SETTINGS);
    if (s_act & ACT_BACK) go(SCR_SETTINGS);
}

static menu_cmd_t update_pause(void) {
    menu_cmd_t cmd = {0};
    int*       cur = &s_cursor[SCR_PAUSE];
    nav(cur, 4);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0: cmd.kind = MENU_CMD_RESUME; break;
            case 1: cmd.kind = MENU_CMD_SAVE; break;
            case 2:
                s_settings_parent = SCR_PAUSE;
                go(SCR_SETTINGS);
                break;
            default: cmd.kind = MENU_CMD_SAVE_QUIT; break;
        }
    } else if (s_act & ACT_BACK) {
        cmd.kind = MENU_CMD_RESUME;
    }
    return cmd;
}

menu_cmd_t menu_update(void) {
    menu_cmd_t cmd = {0};
    switch (s_scr) {
        case SCR_TITLE: cmd = update_title(); break;
        case SCR_WORLDS: cmd = update_worlds(); break;
        case SCR_WORLD: cmd = update_world(); break;
        case SCR_NEW: cmd = update_new(); break;
        case SCR_TEXT: update_text(); break;
        case SCR_DELETE: update_delete(); break;
        case SCR_SETTINGS: update_settings(); break;
        case SCR_CONTROLS: update_controls(); break;
        case SCR_GRAPHICS: cmd = update_graphics(); break;
        case SCR_AUDIO: update_audio(); break;
        case SCR_DISPLAY: update_display(); break;
        case SCR_PAUSE: cmd = update_pause(); break;
        default: break;
    }
    s_act     = 0;
    s_typed_n = 0;
    s_swallow = false;
    return cmd;
}

// --- Drawing ----------------------------------------------------------------

// How many rows a panel shows. Longer lists scroll, which the engine's
// list menu does itself (se_menu_def_t.visible_rows, engine 2.1).
#define VISIBLE 7

static void draw_list(pax_buf_t* fb, char const* title, char const* subtitle, se_menu_row_t const* rows, int n,
                      int cursor, char const* hint, float value_dx) {
    char const*         sub = status_line();
    se_menu_def_t const def = {
        .title        = title,
        .subtitle     = sub != NULL ? sub : subtitle,
        .rows         = rows,
        .row_count    = n,
        .hint         = hint,
        .title_h      = 32.0f,
        .row_h        = 38.0f,
        .value_dx     = value_dx,
        .panel_w      = 0.80f,
        .panel_h      = 0.92f,
        .visible_rows = VISIBLE,
    };
    se_menu_t const m = {.def = &def, .cursor = cursor};
    se_menu_draw(&m, fb);
}

static char const HINT_LIST[]   = "up / down to choose, enter to select, esc to go back";
static char const HINT_ADJUST[] = "up / down to choose, left / right to change, esc to go back";

// The title's own row: a strip along the bottom, under the word, so the
// word stays in view. The rest of the menus are panels over it.
static void draw_title_bar(pax_buf_t* fb) {
    static char const* const ITEMS[TITLE_ROWS] = {"Play", "Settings", "Quit"};
    float const              h                 = 26.0f;
    float const              gap               = 56.0f;
    float                    w[TITLE_ROWS];
    float                    total = 0.0f;
    for (int i = 0; i < TITLE_ROWS; i++) {
        w[i] = rendertext_size(NULL, h, ITEMS[i]).x;
        total += w[i] + (i ? gap : 0.0f);
    }
    float const fw = pax_buf_get_widthf(fb), fh = pax_buf_get_heightf(fb);
    float const y  = fh - 70.0f;
    float       x  = (fw - total) * 0.5f;
    int const   bx = (int)(x - 36.0f), by = (int)(y - 12.0f);
    direct_565_dim_rect((uint16_t*)pax_buf_get_pixels(fb), fb->reverse_endianness, bx, by, (int)(total + 72.0f),
                        (int)(h + 24.0f));
    for (int i = 0; i < TITLE_ROWS; i++) {
        bool const sel = i == s_cursor[SCR_TITLE];
        if (sel) rendertext_draw(fb, SE_UI_COL_HILITE, NULL, h, x - 26.0f, y, ">");
        rendertext_draw(fb, sel ? SE_UI_COL_HILITE : SE_UI_COL_NORMAL, NULL, h, x, y, ITEMS[i]);
        x += w[i] + gap;
    }
    char const* st = status_line();
    if (st != NULL) {
        float const sw = rendertext_size(NULL, 16.0f, st).x;
        rendertext_draw(fb, SE_UI_COL_NORMAL, NULL, 16.0f, (fw - sw) * 0.5f, y - 36.0f, st);
    }
}

void menu_draw(pax_buf_t* fb) {
    if (fb == NULL) return;
    switch (s_scr) {
        case SCR_TITLE: draw_title_bar(fb); break;

        case SCR_WORLDS: {
            static char   labels[CM_SLOTS][CM_WORLD_NAME_MAX + 8];
            se_menu_row_t rows[CM_SLOTS + 1];
            memset(rows, 0, sizeof(rows));
            for (int i = 0; i < CM_SLOTS; i++) {
                if (s_slot_used[i]) {
                    snprintf(labels[i], sizeof(labels[i]), "%d  %s", i + 1, s_slot_meta[i].name);
                } else {
                    snprintf(labels[i], sizeof(labels[i]), "%d  - empty -", i + 1);
                }
                rows[i].label = labels[i];
            }
            rows[CM_SLOTS].label = "Back";
            draw_list(fb, "Worlds", NULL, rows, CM_SLOTS + 1, s_cursor[SCR_WORLDS], HINT_LIST, 0.0f);
        } break;

        case SCR_WORLD: {
            static char sub[48];
            snprintf(sub, sizeof(sub), "slot %d, seed %u", s_slot + 1, (unsigned)s_slot_meta[s_slot].seed);
            se_menu_row_t const rows[4] = {{.label = "Play"}, {.label = "Rename"}, {.label = "Delete"}, {.label = "Back"}};
            draw_list(fb, s_slot_meta[s_slot].name, sub, rows, 4, s_cursor[SCR_WORLD], HINT_LIST, 0.0f);
        } break;

        case SCR_NEW: {
            static char sub[32];
            snprintf(sub, sizeof(sub), "in slot %d", s_slot + 1);
            se_menu_row_t const rows[4] = {
                {.label = "Name", .kind = SE_MENU_VAL_TEXT, .value = s_new_name},
                {.label = "Seed", .kind = SE_MENU_VAL_TEXT, .value = s_new_seed[0] ? s_new_seed : "random"},
                {.label = "Create world"},
                {.label = "Cancel"},
            };
            draw_list(fb, "New world", sub, rows, 4, s_cursor[SCR_NEW], HINT_LIST, 120.0f);
        } break;

        case SCR_TEXT: {
            static char shown[CM_WORLD_NAME_MAX + 2];
            snprintf(shown, sizeof(shown), "%s_", s_text);
            se_menu_row_t const row = {.label = shown};
            draw_list(fb, s_text_title, "type, then enter to keep it", &row, 1, 0,
                      "backspace to delete, esc to cancel", 0.0f);
        } break;

        case SCR_DELETE: {
            static char title[CM_WORLD_NAME_MAX + 12];
            snprintf(title, sizeof(title), "Delete \"%s\"?", s_slot_meta[s_slot].name);
            se_menu_row_t const rows[2] = {{.label = "No, keep it"}, {.label = "Yes, delete it for good"}};
            draw_list(fb, title, "this cannot be undone", rows, 2, s_cursor[SCR_DELETE], HINT_LIST, 0.0f);
        } break;

        case SCR_SETTINGS: {
            se_menu_row_t const rows[SETTINGS_ROWS] = {
                {.label = "Controls"}, {.label = "Graphics"}, {.label = "Audio"}, {.label = "Display"}, {.label = "Back"},
            };
            draw_list(fb, "Settings", NULL, rows, SETTINGS_ROWS, s_cursor[SCR_SETTINGS], HINT_LIST, 0.0f);
        } break;

        case SCR_CONTROLS: {
            se_menu_row_t rows[CONTROLS_ROWS];
            memset(rows, 0, sizeof(rows));
            rows[0].label   = "Gyroscope";
            rows[0].kind    = SE_MENU_VAL_CHECK;
            rows[0].checked = settings_gyro();
            for (int a = 0; a < CM_ACTION_COUNT; a++) {
                se_menu_row_t* r = &rows[CONTROLS_FIRST_KEY + a];
                r->label         = input_action_label((cm_action_t)a);
                r->kind          = SE_MENU_VAL_CUSTOM;
                r->draw_value    = controls_keybind_draw;
                r->ctx           = (void*)(uintptr_t)input_key((cm_action_t)a);
            }
            rows[CONTROLS_FIRST_KEY + CM_ACTION_COUNT].label     = "Reset to defaults";
            rows[CONTROLS_FIRST_KEY + CM_ACTION_COUNT + 1].label = "Back";
            draw_list(fb, "Controls", NULL, rows, CONTROLS_ROWS, s_cursor[SCR_CONTROLS],
                      "up / down to choose, enter to change the key, esc to go back", 260.0f);
        } break;

        case SCR_GRAPHICS: {
            se_menu_row_t const rows[GRAPHICS_ROWS] = {
                {.label = "View distance", .kind = SE_MENU_VAL_TEXT, .value = VIEW_NAMES[settings_view()]},
                {.label = "Textures", .kind = SE_MENU_VAL_CHECK, .checked = settings_textured()},
                {.label = "Resolution",
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = settings_half_res() ? "Half (faster)" : "Full (sharper)"},
                {.label = "Clouds", .kind = SE_MENU_VAL_CHECK, .checked = settings_clouds()},
                {.label = "Camera", .kind = SE_MENU_VAL_TEXT, .value = settings_third_person() ? "Third person" : "First person"},
                {.label = "Fred's hand", .kind = SE_MENU_VAL_TEXT, .value = settings_left_handed() ? "Left" : "Right"},
                {.label = "Back"},
            };
            draw_list(fb, "Graphics", NULL, rows, GRAPHICS_ROWS, s_cursor[SCR_GRAPHICS], HINT_ADJUST, 260.0f);
        } break;

        case SCR_AUDIO: {
            se_menu_row_t const rows[4] = {
                {.label = "Volume", .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_volume()},
                {.label = "Music", .kind = SE_MENU_VAL_CHECK, .checked = settings_music()},
                {.label = "Sound effects", .kind = SE_MENU_VAL_CHECK, .checked = settings_sfx()},
                {.label = "Back"},
            };
            // Honest about it: the toggles are remembered, but there is
            // nothing to hear yet.
            draw_list(fb, "Audio", "no sounds yet: these are kept for when there are", rows, 4,
                      s_cursor[SCR_AUDIO], HINT_ADJUST, 260.0f);
        } break;

        case SCR_DISPLAY: {
            se_menu_row_t const rows[4] = {
                {.label = "Screen", .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_display_brightness()},
                {.label = "Keyboard", .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_keyboard_brightness()},
                {.label = "LEDs", .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_led_brightness()},
                {.label = "Back"},
            };
            draw_list(fb, "Display", "shared with the launcher and every other app", rows, 4, s_cursor[SCR_DISPLAY],
                      HINT_ADJUST, 260.0f);
        } break;

        case SCR_PAUSE: {
            se_menu_row_t const rows[4] = {
                {.label = "Resume"}, {.label = "Save"}, {.label = "Settings"}, {.label = "Save and quit to title"},
            };
            draw_list(fb, "Paused", NULL, rows, 4, s_cursor[SCR_PAUSE], HINT_LIST, 0.0f);
        } break;

        default: break;
    }
}
