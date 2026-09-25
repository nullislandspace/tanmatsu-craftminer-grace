// =====================================================================
//  SynthMiner  --  the menus (see menu.h)
// =====================================================================

#include "audio/sfx.h"
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
#include "i18n/i18n.h"
#include "world/datadir.h"
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
    SCR_LANGUAGE,  // which language the UI is in
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
static world_meta_t s_slot_meta[SM_SLOTS];
static slot_state_t s_slot_state[SM_SLOTS];
static int          s_slot;  // the slot the WORLD / NEW / DELETE screens are about

// The new-world form.
static char s_new_name[SM_WORLD_NAME_MAX];
static char s_new_seed[12];

// Typing. The text is edited in a copy and only written back on Enter,
// so Esc really does leave the old value alone.
typedef enum { FIELD_NEW_NAME = 0, FIELD_NEW_SEED, FIELD_RENAME } field_t;
static field_t     s_field;
static char        s_text[SM_WORLD_NAME_MAX];
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
    for (int i = 0; i < SM_SLOTS; i++) {
        s_slot_state[i] = worldstore_slot_state(i, &s_slot_meta[i]);
        // A damaged world still gets the world screen -- to delete it --
        // under a name that says what it is.
        if (s_slot_state[i] == SLOT_DAMAGED) snprintf(s_slot_meta[i].name, sizeof(s_slot_meta[i].name), "%s", T(SM_STR_WORLDS_DAMAGED));
    }
}

static int clampi(int v, int lo, int hi) {
    return v < lo ? lo : v > hi ? hi : v;
}

// Up and down over `n` rows. No wrap, like the engine's own menus.
//
// The click is here rather than in the key decoding so it follows the
// CURSOR, not the key: pressing Up on the top row moves nothing and so
// says nothing, which is the difference between a menu that responds and
// one that rattles.
static void nav(int* cursor, int n) {
    int const was = *cursor;
    if (s_act & ACT_UP) (*cursor)--;
    if (s_act & ACT_DOWN) (*cursor)++;
    *cursor = clampi(*cursor, 0, n - 1);
    if (*cursor != was) sfx_play(SFX_CLICK);
}

static uint8_t pct_step(uint8_t cur, int delta) {
    return (uint8_t)clampi((int)cur + delta, 0, 100);
}

// A seed from what was typed: a number is that number, anything else
// is hashed (so "Kurt" is a seed, the way it is in Minecraft), and
// nothing at all is random.
static uint32_t seed_from(char const* t) {
    if (t[0] == '\0') {
        return (uint32_t)sm_mix64((uint64_t)esp_timer_get_time() ^ 0x9E3779B97F4A7C15ull);
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
        {"language", SCR_LANGUAGE},
    };
    for (size_t i = 0; i < sizeof(SHOW) / sizeof(SHOW[0]); i++) {
        if (strcmp(name, SHOW[i].name) != 0) continue;
        refresh_slots();
        s_slot = 0;
        i18n_fmt(s_new_name, sizeof(s_new_name), SM_STR_NEW_DEFAULT_NAME, 2);
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
    nav(cur, SM_SLOTS + 1);
    if (s_act & ACT_OK) {
        if (*cur == SM_SLOTS) {
            go(SCR_TITLE);
        } else if (s_slot_state[*cur] == SLOT_NEWER) {
            // Somebody's world, from a later build. Not ours to open, and
            // not free either: nothing is offered.
            menu_status(T(SM_STR_STATUS_NEWER));
        } else if (s_slot_state[*cur] == SLOT_OLDER) {
            menu_status(T(SM_STR_STATUS_OLDER));
        } else if (s_slot_state[*cur] != SLOT_EMPTY) {
            s_slot               = *cur;
            s_cursor[SCR_WORLD] = 0;
            go(SCR_WORLD);
        } else {
            s_slot = *cur;
            i18n_fmt(s_new_name, sizeof(s_new_name), SM_STR_NEW_DEFAULT_NAME, *cur + 1);
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
                begin_text(FIELD_RENAME, T(SM_STR_TEXT_TITLE_RENAME), s_slot_meta[s_slot].name, SM_WORLD_NAME_MAX, SCR_WORLD);
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
            case 0: begin_text(FIELD_NEW_NAME, T(SM_STR_TEXT_TITLE_NAME), s_new_name, SM_WORLD_NAME_MAX, SCR_NEW); break;
            case 1:
                begin_text(FIELD_NEW_SEED, T(SM_STR_TEXT_TITLE_SEED), s_new_seed, (int)sizeof(s_new_seed), SCR_NEW);
                break;
            case 2:
                if (s_new_name[0] == '\0') {
                    menu_status(T(SM_STR_STATUS_NEEDS_NAME));
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
                    menu_status(T(SM_STR_STATUS_RENAMED));
                } else {
                    menu_status(T(SM_STR_STATUS_RENAME_FAILED));
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
            menu_status(T(ok ? SM_STR_STATUS_DELETED : SM_STR_STATUS_DELETE_FAILED));
            refresh_slots();
            go(SCR_WORLDS);
        } else {
            go(SCR_WORLD);
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_WORLD);
    }
}

#define SETTINGS_ROWS 6

static void update_settings(void) {
    int* cur = &s_cursor[SCR_SETTINGS];
    nav(cur, SETTINGS_ROWS);
    if (s_act & ACT_OK) {
        switch (*cur) {
            case 0: go(SCR_LANGUAGE); break;
            case 1: go(SCR_CONTROLS); break;
            case 2: go(SCR_GRAPHICS); break;
            case 3: go(SCR_AUDIO); break;
            case 4: go(SCR_DISPLAY); break;
            default: go(s_settings_parent); break;
        }
    } else if (s_act & ACT_BACK) {
        go(s_settings_parent);
    }
}

// Every language, then Back. Choosing one takes effect on the next frame
// -- every label is fetched where it is drawn -- and is written to
// settings.txt at once, the way every other setting is.
#define LANGUAGE_ROWS (SM_LANG_COUNT + 1)

static void update_language(void) {
    int* cur = &s_cursor[SCR_LANGUAGE];
    nav(cur, LANGUAGE_ROWS);
    if (s_act & ACT_OK) {
        if (*cur < SM_LANG_COUNT) {
            i18n_set_language((sm_lang_t)*cur);
            i18n_load_overrides(SM_DATA_DIR);
            settings_save();
            ESP_LOGI(TAG, "language: %s", i18n_language_code(i18n_language()));
        }
        go(SCR_SETTINGS);
    } else if (s_act & ACT_BACK) {
        go(SCR_SETTINGS);
    }
}

// Rows: the gyroscope checkbox (first, as in synthracer), every action,
// then "Reset to defaults", then "Back".
#define CONTROLS_FIRST_KEY 1
#define CONTROLS_ROWS      (CONTROLS_FIRST_KEY + SM_ACTION_COUNT + 2)

static void update_controls(void) {
    int* cur = &s_cursor[SCR_CONTROLS];
    nav(cur, CONTROLS_ROWS);
    if ((s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) && *cur == 0) {
        settings_set_gyro(!settings_gyro());
    } else if (s_act & ACT_OK) {
        int const key = *cur - CONTROLS_FIRST_KEY;
        if (key < SM_ACTION_COUNT) {
            // The engine's blocking "press a key" capture, as synthracer
            // uses it. It takes any key, Esc and the cursor keys included
            // (the cursor keys since engine 2.1), so there is no cancel:
            // pressing the key it already had keeps it.
            sm_action_t const a  = (sm_action_t)key;
            uint16_t const    sc = se_ui_capture_key(input_action_label(a));
            if (sc != 0) {
                char name[24];
                input_bind(a, sc);
                ESP_LOGI(TAG, "%s bound to %s", input_action_label(a), input_key_name(sc, name, sizeof(name)));
            }
        } else if (key == SM_ACTION_COUNT) {
            input_reset_defaults();
            menu_status(T(SM_STR_STATUS_KEYS_RESET));
        } else {
            go(SCR_SETTINGS);
        }
    } else if (s_act & ACT_BACK) {
        go(SCR_SETTINGS);
    }
}

// Fetched when drawn, not once: the language can change under them.
static sm_str_t const VIEW_NAMES[SETTINGS_VIEW_COUNT] = {SM_STR_VIEW_NEAR, SM_STR_VIEW_MEDIUM,
                                                         SM_STR_VIEW_FAR};
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

// Three sliders, and they are not the same kind of thing. The first is
// the BADGE's volume -- the launcher's setting, shared with every app,
// which is why it goes through se_hw and not through ours. The other two
// are how loudly this game mixes its own music and its own effects, and
// they live in settings.txt. Turning the badge down quietens everything;
// turning Music down leaves the footsteps where they were.
#define AUDIO_ROWS 6

static void update_audio(void) {
    int* cur = &s_cursor[SCR_AUDIO];
    nav(cur, AUDIO_ROWS);
    int const step = (s_act & ACT_RIGHT) ? SE_HW_VOLUME_STEP_PCT : (s_act & ACT_LEFT) ? -SE_HW_VOLUME_STEP_PCT : 0;
    switch (*cur) {
        case 0:
            if (step != 0) se_hw_set_volume(pct_step(se_hw_get_volume(), step));
            break;
        case 1:
            if (s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) settings_set_music(!settings_music());
            break;
        case 2:
            if (step != 0) settings_set_music_volume(pct_step(settings_music_volume(), step));
            break;
        case 3:
            if (s_act & (ACT_OK | ACT_LEFT | ACT_RIGHT)) settings_set_sfx(!settings_sfx());
            break;
        case 4:
            if (step != 0) {
                settings_set_sfx_volume(pct_step(settings_sfx_volume(), step));
                // Play one, so the slider is heard rather than read.
                sfx_play(SFX_PLACE);
            }
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
        case SCR_LANGUAGE: update_language(); break;
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

// The ordinary panel: eight tenths of the screen, and a value column
// 0.80 * 800 wide is room for about 330 px of label.
#define PANEL_W_NORMAL 0.80f
// ... and a wide one, for a screen whose labels are long in some
// language. "Volumen de los efectos" is 374 px at the row height, and
// the slider beside it wants 250 more; the two do not both fit in the
// ordinary panel in any arrangement (F-76).
#define PANEL_W_WIDE 0.94f

static void draw_list_w(pax_buf_t* fb, char const* title, char const* subtitle, se_menu_row_t const* rows, int n,
                        int cursor, char const* hint, float value_dx, float panel_w) {
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
        .panel_w      = panel_w,
        .panel_h      = 0.92f,
        .visible_rows = VISIBLE,
    };
    se_menu_t const m = {.def = &def, .cursor = cursor};
    se_menu_draw(&m, fb);
}

static void draw_list(pax_buf_t* fb, char const* title, char const* subtitle, se_menu_row_t const* rows, int n,
                      int cursor, char const* hint, float value_dx) {
    draw_list_w(fb, title, subtitle, rows, n, cursor, hint, value_dx, PANEL_W_NORMAL);
}

// The same list, packed tighter. For the languages: there are 32 of them,
// and at the ordinary seven rows a panel a player would be scrolling past
// two dozen names to reach their own. Smaller rows show half the list at
// once, which is what makes an alphabetical list worth being alphabetical.
#define VISIBLE_DENSE 13

static void draw_list_dense(pax_buf_t* fb, char const* title, se_menu_row_t const* rows, int n, int cursor,
                            char const* hint, float value_dx) {
    char const*         sub = status_line();
    se_menu_def_t const def = {
        .title        = title,
        .subtitle     = sub,
        .rows         = rows,
        .row_count    = n,
        .hint         = hint,
        .title_h      = 28.0f,
        .row_h        = 27.0f,
        .value_dx     = value_dx,
        .panel_w      = 0.80f,
        .panel_h      = 0.94f,
        .visible_rows = VISIBLE_DENSE,
    };
    se_menu_t const m = {.def = &def, .cursor = cursor};
    se_menu_draw(&m, fb);
}

#define HINT_LIST   T(SM_STR_HINT_LIST)
#define HINT_ADJUST T(SM_STR_HINT_ADJUST)

// The title's own row: a strip along the bottom, under the word, so the
// word stays in view. The rest of the menus are panels over it.
static void draw_title_bar(pax_buf_t* fb) {
    static sm_str_t const    ITEMS[TITLE_ROWS] = {SM_STR_MENU_PLAY, SM_STR_MENU_SETTINGS,
                                                  SM_STR_MENU_QUIT};
    float const              h                 = 26.0f;
    float const              gap               = 56.0f;
    float                    w[TITLE_ROWS];
    float                    total = 0.0f;
    for (int i = 0; i < TITLE_ROWS; i++) {
        w[i] = rendertext_size(NULL, h, T(ITEMS[i])).x;
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
        rendertext_draw(fb, sel ? SE_UI_COL_HILITE : SE_UI_COL_NORMAL, NULL, h, x, y, T(ITEMS[i]));
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
            static char   labels[SM_SLOTS][SM_WORLD_NAME_MAX + 8];
            se_menu_row_t rows[SM_SLOTS + 1];
            memset(rows, 0, sizeof(rows));
            for (int i = 0; i < SM_SLOTS; i++) {
                switch (s_slot_state[i]) {
                    case SLOT_WORLD:
                    case SLOT_DAMAGED:
                        i18n_fmt(labels[i], sizeof(labels[i]), SM_STR_WORLDS_SLOT, i + 1,
                                 s_slot_meta[i].name);
                        break;
                    case SLOT_NEWER:
                        i18n_fmt(labels[i], sizeof(labels[i]), SM_STR_WORLDS_SLOT_NEWER, i + 1);
                        break;
                    case SLOT_OLDER:
                        i18n_fmt(labels[i], sizeof(labels[i]), SM_STR_WORLDS_SLOT_OLDER, i + 1);
                        break;
                    default: i18n_fmt(labels[i], sizeof(labels[i]), SM_STR_WORLDS_SLOT_EMPTY, i + 1); break;
                }
                rows[i].label = labels[i];
            }
            rows[SM_SLOTS].label = T(SM_STR_COMMON_BACK);
            draw_list(fb, T(SM_STR_WORLDS_TITLE), NULL, rows, SM_SLOTS + 1, s_cursor[SCR_WORLDS], HINT_LIST, 0.0f);
        } break;

        case SCR_WORLD: {
            static char sub[48];
            i18n_fmt(sub, sizeof(sub), SM_STR_WORLD_SUB, s_slot + 1, (unsigned)s_slot_meta[s_slot].seed);
            se_menu_row_t const rows[4] = {{.label = T(SM_STR_WORLD_PLAY)},
                                           {.label = T(SM_STR_WORLD_RENAME)},
                                           {.label = T(SM_STR_WORLD_DELETE)},
                                           {.label = T(SM_STR_COMMON_BACK)}};
            draw_list(fb, s_slot_meta[s_slot].name, sub, rows, 4, s_cursor[SCR_WORLD], HINT_LIST, 0.0f);
        } break;

        case SCR_NEW: {
            static char sub[32];
            i18n_fmt(sub, sizeof(sub), SM_STR_NEW_SUB, s_slot + 1);
            se_menu_row_t const rows[4] = {
                {.label = T(SM_STR_NEW_NAME), .kind = SE_MENU_VAL_TEXT, .value = s_new_name},
                {.label = T(SM_STR_NEW_SEED),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = s_new_seed[0] ? s_new_seed : T(SM_STR_NEW_SEED_RANDOM)},
                {.label = T(SM_STR_NEW_CREATE)},
                {.label = T(SM_STR_NEW_CANCEL)},
            };
            draw_list(fb, T(SM_STR_NEW_TITLE), sub, rows, 4, s_cursor[SCR_NEW], HINT_LIST, 120.0f);
        } break;

        case SCR_TEXT: {
            static char shown[SM_WORLD_NAME_MAX + 2];
            snprintf(shown, sizeof(shown), "%s_", s_text);
            se_menu_row_t const row = {.label = shown};
            draw_list(fb, s_text_title, T(SM_STR_TEXT_SUB), &row, 1, 0, T(SM_STR_TEXT_HINT), 0.0f);
        } break;

        case SCR_DELETE: {
            static char title[SM_WORLD_NAME_MAX + 12];
            i18n_fmt(title, sizeof(title), SM_STR_DELETE_TITLE, s_slot_meta[s_slot].name);
            se_menu_row_t const rows[2] = {{.label = T(SM_STR_DELETE_NO)}, {.label = T(SM_STR_DELETE_YES)}};
            draw_list(fb, title, T(SM_STR_DELETE_SUB), rows, 2, s_cursor[SCR_DELETE], HINT_LIST, 0.0f);
        } break;

        case SCR_SETTINGS: {
            se_menu_row_t const rows[SETTINGS_ROWS] = {
                {.label = T(SM_STR_SETTINGS_LANGUAGE),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = i18n_language_name(i18n_language())},
                {.label = T(SM_STR_SETTINGS_CONTROLS)},
                {.label = T(SM_STR_SETTINGS_GRAPHICS)},
                {.label = T(SM_STR_SETTINGS_AUDIO)},
                {.label = T(SM_STR_SETTINGS_DISPLAY)},
                {.label = T(SM_STR_COMMON_BACK)},
            };
            draw_list(fb, T(SM_STR_SETTINGS_TITLE), NULL, rows, SETTINGS_ROWS, s_cursor[SCR_SETTINGS],
                      HINT_LIST, 260.0f);
        } break;

        case SCR_LANGUAGE: {
            // Each language stands in its OWN name, never translated: the
            // player who needs this screen is the one who cannot read the
            // language the game is in.
            se_menu_row_t rows[SM_LANG_COUNT + 1];
            memset(rows, 0, sizeof(rows));
            for (int i = 0; i < SM_LANG_COUNT; i++) {
                rows[i].label = i18n_language_name((sm_lang_t)i);
                // A radio, not a tick: one of these is the language, and
                // choosing another unchooses this one (engine 2.1).
                rows[i].kind    = SE_MENU_VAL_RADIO;
                rows[i].checked = i == (int)i18n_language();
            }
            rows[SM_LANG_COUNT].label = T(SM_STR_COMMON_BACK);
            // 280, not the usual 180: "Nederlands" and "Български" are
            // longer than any label English has, and the tick belongs
            // clear of them.
            draw_list_dense(fb, T(SM_STR_LANGUAGE_TITLE), rows, LANGUAGE_ROWS, s_cursor[SCR_LANGUAGE],
                            HINT_LIST, 280.0f);
        } break;

        case SCR_CONTROLS: {
            se_menu_row_t rows[CONTROLS_ROWS];
            memset(rows, 0, sizeof(rows));
            rows[0].label   = T(SM_STR_CONTROLS_GYRO);
            rows[0].kind    = SE_MENU_VAL_CHECK;
            rows[0].checked = settings_gyro();
            for (int a = 0; a < SM_ACTION_COUNT; a++) {
                se_menu_row_t* r = &rows[CONTROLS_FIRST_KEY + a];
                r->label         = input_action_label((sm_action_t)a);
                r->kind          = SE_MENU_VAL_CUSTOM;
                r->draw_value    = controls_keybind_draw;
                r->ctx           = (void*)(uintptr_t)input_key((sm_action_t)a);
            }
            rows[CONTROLS_FIRST_KEY + SM_ACTION_COUNT].label     = T(SM_STR_CONTROLS_RESET);
            rows[CONTROLS_FIRST_KEY + SM_ACTION_COUNT + 1].label = T(SM_STR_COMMON_BACK);
            draw_list(fb, T(SM_STR_CONTROLS_TITLE), NULL, rows, CONTROLS_ROWS, s_cursor[SCR_CONTROLS],
                      T(SM_STR_CONTROLS_HINT), 260.0f);
        } break;

        case SCR_GRAPHICS: {
            se_menu_row_t const rows[GRAPHICS_ROWS] = {
                {.label = T(SM_STR_GRAPHICS_VIEW),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = T(VIEW_NAMES[settings_view()])},
                {.label = T(SM_STR_GRAPHICS_TEXTURES), .kind = SE_MENU_VAL_CHECK, .checked = settings_textured()},
                {.label = T(SM_STR_GRAPHICS_RESOLUTION),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = T(settings_half_res() ? SM_STR_RES_HALF : SM_STR_RES_FULL)},
                {.label = T(SM_STR_GRAPHICS_CLOUDS), .kind = SE_MENU_VAL_CHECK, .checked = settings_clouds()},
                {.label = T(SM_STR_GRAPHICS_CAMERA),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = T(settings_third_person() ? SM_STR_CAMERA_THIRD : SM_STR_CAMERA_FIRST)},
                {.label = T(SM_STR_GRAPHICS_HAND),
                 .kind  = SE_MENU_VAL_TEXT,
                 .value = T(settings_left_handed() ? SM_STR_HAND_LEFT : SM_STR_HAND_RIGHT)},
                {.label = T(SM_STR_COMMON_BACK)},
            };
            draw_list_w(fb, T(SM_STR_GRAPHICS_TITLE), NULL, rows, GRAPHICS_ROWS, s_cursor[SCR_GRAPHICS],
                        HINT_ADJUST, 340.0f, PANEL_W_WIDE);
        } break;

        case SCR_AUDIO: {
            se_menu_row_t const rows[AUDIO_ROWS] = {
                {.label = T(SM_STR_AUDIO_VOLUME), .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_volume()},
                {.label = T(SM_STR_AUDIO_MUSIC), .kind = SE_MENU_VAL_CHECK, .checked = settings_music()},
                {.label     = T(SM_STR_AUDIO_MUSIC_VOLUME),
                 .kind      = SE_MENU_VAL_RANGE,
                 .range_pct = settings_music_volume()},
                {.label = T(SM_STR_AUDIO_SFX), .kind = SE_MENU_VAL_CHECK, .checked = settings_sfx()},
                {.label     = T(SM_STR_AUDIO_SFX_VOLUME),
                 .kind      = SE_MENU_VAL_RANGE,
                 .range_pct = settings_sfx_volume()},
                {.label = T(SM_STR_COMMON_BACK)},
            };
            draw_list_w(fb, T(SM_STR_AUDIO_TITLE), T(SM_STR_AUDIO_SUB), rows, AUDIO_ROWS,
                        s_cursor[SCR_AUDIO], HINT_ADJUST, 390.0f, PANEL_W_WIDE);
        } break;

        case SCR_DISPLAY: {
            se_menu_row_t const rows[4] = {
                {.label     = T(SM_STR_DISPLAY_SCREEN),
                 .kind      = SE_MENU_VAL_RANGE,
                 .range_pct = se_hw_get_display_brightness()},
                {.label     = T(SM_STR_DISPLAY_KEYBOARD),
                 .kind      = SE_MENU_VAL_RANGE,
                 .range_pct = se_hw_get_keyboard_brightness()},
                {.label = T(SM_STR_DISPLAY_LEDS), .kind = SE_MENU_VAL_RANGE, .range_pct = se_hw_get_led_brightness()},
                {.label = T(SM_STR_COMMON_BACK)},
            };
            draw_list(fb, T(SM_STR_DISPLAY_TITLE), T(SM_STR_DISPLAY_SUB), rows, 4, s_cursor[SCR_DISPLAY],
                      HINT_ADJUST, 300.0f);
        } break;

        case SCR_PAUSE: {
            se_menu_row_t const rows[4] = {
                {.label = T(SM_STR_PAUSE_RESUME)},
                {.label = T(SM_STR_PAUSE_SAVE)},
                {.label = T(SM_STR_SETTINGS_TITLE)},
                {.label = T(SM_STR_PAUSE_QUIT)},
            };
            draw_list(fb, T(SM_STR_PAUSE_TITLE), NULL, rows, 4, s_cursor[SCR_PAUSE], HINT_LIST, 0.0f);
        } break;

        default: break;
    }
}
