// =====================================================================
//  CraftMiner  --  the crafting book (see craft_ui.h)
// =====================================================================

#include "ui/craft_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "i18n/fold.h"
#include "i18n/i18n.h"
#include "items/recipes.h"
#include "se_ui.h"
#include "testkit/showtime.h"

// The most rows the book can show at once. There will never be this
// many recipes; the array is the filter's working space and is cheap.
#define CRAFT_ROWS_MAX 64
#define VISIBLE        8

// How long "Made 4 Torch" stays under the panel.
#define MSG_SECONDS 1.8

static bool s_open;
static int  s_station;
static int  s_cursor;

// What was typed, and the same folded down to what the keyboard can
// reach. Both are kept: the player reads the first, the filter uses the
// second (fold.h).
static char s_query[FOLD_MAX];
static char s_folded[FOLD_MAX];

// This frame's keys, gathered from events the way menu.c does it: a
// press can arrive as a scancode AND a navigation AND a character, and
// it must move the cursor exactly once.
#define ACT_UP    0x01u
#define ACT_DOWN  0x02u
#define ACT_OK    0x04u
#define ACT_BACK  0x08u
#define ACT_BKSP  0x10u
#define ACT_LEFT  0x20u
#define ACT_RIGHT 0x40u
static unsigned s_act;
static char     s_typed[16];
static int      s_typed_n;

static char   s_msg[64];
static double s_msg_until;

// THE KEY THAT OPENS THE BOOK ALSO TYPES ITS LETTER. A press arrives as
// a scancode and then as a character, both in the same drain of the
// input queue -- so the C that opened this screen turned up in the
// search box (the user found it on the first try). Everything typed
// before the first update after opening is dropped, which is exactly
// the opening key and nothing a player could have meant.
static bool s_eat_chars;

// The cursor's recipe, spelled out: what it takes, and how much of each
// is carried. The user's ask -- "missing" on its own does not say what
// is missing, so there has to be somewhere that does.
static bool s_detail;

// The filtered list, rebuilt every frame: recipe_count() is small and a
// cache would be one more thing to invalidate when the inventory moves.
static int s_list[CRAFT_ROWS_MAX];
static int s_list_n;
static int s_known_n;  // known before the search box narrowed it

void craft_ui_open(int station) {
    s_open      = true;
    s_station   = station;
    s_cursor    = 0;
    s_query[0]  = '\0';
    s_folded[0] = '\0';
    s_act       = 0;
    s_typed_n   = 0;
    s_msg_until = 0.0;
    s_eat_chars = true;
    s_detail    = false;
}

void craft_ui_close(void) {
    s_open = false;
}

bool craft_ui_active(void) {
    return s_open;
}

void craft_ui_event(bsp_input_event_t const* ev) {
    if (!s_open || ev == NULL) return;

    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: s_act |= ACT_OK; break;
            case BSP_INPUT_NAVIGATION_KEY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_NAVIGATION_KEY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: s_act |= ACT_BKSP; break;
            case BSP_INPUT_NAVIGATION_KEY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_NAVIGATION_KEY_RIGHT: s_act |= ACT_RIGHT; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if ((sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return;
        switch (sc) {
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT: s_act |= ACT_RIGHT; break;
            case BSP_INPUT_SCANCODE_BACKSPACE: s_act |= ACT_BKSP; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_KEYBOARD) {
        // Printable only. Enter, Backspace and Esc also send a
        // character on some keyboards, and they are commands here.
        char const c = ev->args_keyboard.ascii;
        if (c >= 32 && c < 127 && s_typed_n < (int)sizeof(s_typed)) s_typed[s_typed_n++] = c;
    }
}

// A CRAFTING TABLE CAN DO EVERYTHING THE HANDS CAN, and not the other
// way round (the user's catch: torches and planks belong in both).
// It is a one-way widening, not a set of equals -- standing at a table
// must never be a reason to walk away from it.
static bool station_shows(uint8_t station) {
    if (station == s_station) return true;
    return s_station == RS_TABLE && station == RS_INVENTORY;
}

// Rebuild s_list from what the player knows and what they typed.
static void filter(inventory_t const* inv) {
    s_list_n  = 0;
    s_known_n = 0;
    for (int i = 0; i < recipe_count() && s_list_n < CRAFT_ROWS_MAX; i++) {
        recipe_t const* r = recipe_at(i);
        if (!station_shows(r->station)) continue;
        if (!recipe_known(r, inv)) continue;
        s_known_n++;
        // Matched against the name the player READS, folded -- and
        // against the stable English id as well, so "pick" finds the
        // pickaxe whatever the language is set to. That is also the way
        // out for somebody who set the language to Greek by accident.
        if (!fold_match(T(item_label(r->out)), s_folded) &&
            !fold_match(item_def(r->out).name, s_folded)) {
            continue;
        }
        s_list[s_list_n++] = i;
    }
}

void craft_ui_update(inventory_t* inv) {
    if (!s_open || inv == NULL) return;

    if (s_eat_chars) {
        s_typed_n   = 0;
        s_eat_chars = false;
    }

    // --- What it takes ------------------------------------------------
    //
    // A second screen over the first, and while it is up it owns the
    // keys -- including the letters, which do NOT go on filtering
    // behind it.
    if (s_detail) {
        if (s_act & (ACT_BACK | ACT_LEFT | ACT_OK)) s_detail = false;
        s_typed_n = 0;
        s_act     = 0;
        filter(inv);
        if (s_cursor >= s_list_n) s_detail = false;
        return;
    }

    // --- The search box ----------------------------------------------
    int len = (int)strlen(s_query);
    for (int i = 0; i < s_typed_n && len + 1 < (int)sizeof(s_query); i++) {
        s_query[len++] = s_typed[i];
        s_query[len]   = '\0';
    }
    if ((s_act & ACT_BKSP) && len > 0) s_query[--len] = '\0';
    if (s_typed_n > 0 || (s_act & ACT_BKSP)) {
        fold_text(s_query, s_folded, sizeof(s_folded));
        s_cursor = 0;  // a narrowed list under an old cursor points at nothing
    }
    s_typed_n = 0;

    filter(inv);

    // --- Moving and making -------------------------------------------
    if (s_act & ACT_BACK) {
        s_open = false;
        s_act  = 0;
        return;
    }
    if (s_list_n > 0) {
        if (s_act & ACT_UP) s_cursor--;
        if (s_act & ACT_DOWN) s_cursor++;
        if (s_cursor < 0) s_cursor = 0;
        if (s_cursor >= s_list_n) s_cursor = s_list_n - 1;

        // Right always opens the detail; enter opens it only when there
        // is nothing to make, because then it is the useful answer to
        // the key rather than a refusal.
        if (s_act & ACT_RIGHT) s_detail = true;

        if (!s_detail && (s_act & ACT_OK)) {
            recipe_t const* r = recipe_at(s_list[s_cursor]);
            if (recipe_can_make(r, inv, 1) < 1) {
                s_detail = true;
                sfx_play(SFX_DENY);
            } else if (recipe_make(r, inv, 1) > 0) {
                i18n_fmt(s_msg, sizeof(s_msg), CM_STR_CRAFT_MADE, (int)r->out_n, T(item_label(r->out)));
                s_msg_until = showtime_now() + MSG_SECONDS;
                sfx_play(SFX_CRAFT);
            } else {
                // Everything was there and it still did not happen, so
                // the output had nowhere to go (recipes.c keeps the
                // ingredients in that case).
                snprintf(s_msg, sizeof(s_msg), "%s", T(CM_STR_CRAFT_FULL));
                s_msg_until = showtime_now() + MSG_SECONDS;
                sfx_play(SFX_DENY);
            }
        }
    } else {
        s_cursor = 0;
    }
    s_act = 0;
}

// The line under the panel: what the cursor's recipe needs, and how
// much of it is carried.
static void ingredients_line(recipe_t const* r, inventory_t const* inv, char* out, size_t cap) {
    out[0] = '\0';
    for (int i = 0; i < r->n_in; i++) {
        char      part[64];
        int const have = inv_count(inv, r->in[i].item);
        i18n_fmt(part, sizeof(part), CM_STR_CRAFT_ING, T(item_label(r->in[i].item)), have,
                 (int)r->in[i].count);
        if (out[0] != '\0') strncat(out, "   ", cap - strlen(out) - 1);
        strncat(out, part, cap - strlen(out) - 1);
    }
}

// The "what it takes" screen: one row per ingredient, each saying how
// many are needed and how many are carried. This is the whole answer to
// a row that says only "missing".
static void draw_detail(pax_buf_t* fb, inventory_t const* inv) {
    recipe_t const* r = recipe_at(s_list[s_cursor]);

    se_menu_row_t rows[RECIPE_IN_MAX];
    static char   labels[RECIPE_IN_MAX][64];
    static char   vals[RECIPE_IN_MAX][48];

    for (int i = 0; i < r->n_in; i++) {
        int const have = inv_count(inv, r->in[i].item);
        // "have / needed", and NOT a sentence. The first version read
        // "have 0, need 2 more", which in the value column ran off the
        // panel and off the screen behind it -- and would have run
        // further in every language with longer words than English.
        // Two numbers cannot do that, and say the same thing.
        snprintf(labels[i], sizeof(labels[i]), "%s", T(item_label(r->in[i].item)));
        i18n_fmt(vals[i], sizeof(vals[i]), CM_STR_CRAFT_DETAIL_HAVE, have, (int)r->in[i].count);
        memset(&rows[i], 0, sizeof(rows[i]));
        rows[i].label = labels[i];
        rows[i].kind  = SE_MENU_VAL_TEXT;
        rows[i].value = vals[i];
    }

    se_menu_def_t const def = {
        .title     = T(item_label(r->out)),
        .subtitle  = T(CM_STR_CRAFT_DETAIL_SUB),
        .rows      = rows,
        .row_count = r->n_in,
        .hint      = T(CM_STR_CRAFT_DETAIL_HINT),
        .title_h   = 32.0f,
        .row_h     = 38.0f,
        .value_dx  = 300.0f,
        .panel_w   = 0.78f,
        .panel_h   = 0.62f,
    };
    // No cursor: nothing here is chosen, it is a thing to read. Past the
    // last row, so no line is highlighted as if it were.
    se_menu_t const m = {.def = &def, .cursor = r->n_in};
    se_menu_draw(&m, fb);
}

void craft_ui_draw(pax_buf_t* fb, inventory_t const* inv) {
    if (!s_open || inv == NULL) return;

    if (s_detail && s_list_n > 0 && s_cursor < s_list_n) {
        draw_detail(fb, inv);
        return;
    }

    se_menu_row_t rows[CRAFT_ROWS_MAX];
    static char   vals[CRAFT_ROWS_MAX][24];
    int           n = 0;

    for (int i = 0; i < s_list_n; i++) {
        recipe_t const* r = recipe_at(s_list[i]);
        if (recipe_can_make(r, inv, 1) >= 1) {
            i18n_fmt(vals[n], sizeof(vals[n]), CM_STR_CRAFT_VALUE_MAKE, (int)r->out_n);
        } else {
            snprintf(vals[n], sizeof(vals[n]), "%s", T(CM_STR_CRAFT_VALUE_MISSING));
        }
        rows[n].label      = T(item_label(r->out));
        rows[n].kind       = SE_MENU_VAL_TEXT;
        rows[n].value      = vals[n];
        rows[n].checked    = false;
        rows[n].range_pct  = 0;
        rows[n].draw_value = NULL;
        rows[n].ctx        = NULL;
        n++;
    }

    // Nothing to show, and the two reasons for it are different news:
    // an empty book is the game telling you to go and find something,
    // an empty filter is you having mistyped.
    if (n == 0) {
        memset(&rows[0], 0, sizeof(rows[0]));
        rows[0].label = T(s_known_n == 0 ? CM_STR_CRAFT_EMPTY : CM_STR_CRAFT_NO_MATCH);
        rows[0].kind  = SE_MENU_VAL_NONE;
        n             = 1;
    }

    char search[FOLD_MAX + 32];
    i18n_fmt(search, sizeof(search), CM_STR_CRAFT_SEARCH, s_query);
    // A caret, so an empty box still looks like something you type into.
    strncat(search, "_", sizeof(search) - strlen(search) - 1);

    // The footer carries the cursor's ingredients -- or, for a moment
    // after crafting, what just happened.
    char footer[192];
    if (showtime_now() < s_msg_until) {
        snprintf(footer, sizeof(footer), "%s", s_msg);
    } else if (s_list_n > 0) {
        ingredients_line(recipe_at(s_list[s_cursor]), inv, footer, sizeof(footer));
    } else {
        snprintf(footer, sizeof(footer), "%s", T(s_known_n == 0 ? CM_STR_CRAFT_EMPTY_SUB : CM_STR_CRAFT_HINT));
    }

    se_menu_def_t const def = {
        .title        = T(s_station == RS_TABLE ? CM_STR_CRAFT_TITLE_TABLE : CM_STR_CRAFT_TITLE),
        .subtitle     = search,
        .rows         = rows,
        .row_count    = n,
        .hint         = footer,
        .title_h      = 32.0f,
        .row_h        = 34.0f,
        .value_dx     = 300.0f,
        .panel_w      = 0.72f,
        .panel_h      = 0.92f,
        .visible_rows = VISIBLE,
    };
    se_menu_t const m = {.def = &def, .cursor = s_list_n > 0 ? s_cursor : 0};
    se_menu_draw(&m, fb);
}
