// =====================================================================
//  SynthMiner  --  the cheat console (see cheat_ui.h)
// =====================================================================

#include "ui/cheat_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "i18n/fold.h"
#include "items/items.h"
#include "se_ui.h"
#include "testkit/showtime.h"

#define CHEAT_MAX 64
#define VISIBLE   9
#define MSG_SECONDS 1.8

static bool s_open;
static int  s_cursor;
static char s_query[FOLD_MAX];
static char s_folded[FOLD_MAX];

// How many to give. The amounts a player actually wants, rather than a
// number to count up to with an arrow key.
static int const AMOUNTS[] = {1, 8, 16, 32, 64};
#define AMOUNT_N ((int)(sizeof(AMOUNTS) / sizeof(AMOUNTS[0])))
static int s_amount = AMOUNT_N - 1;

static uint16_t s_list[CHEAT_MAX];
static int      s_list_n;

#define ACT_UP    0x01u
#define ACT_DOWN  0x02u
#define ACT_LEFT  0x04u
#define ACT_RIGHT 0x08u
#define ACT_OK    0x10u
#define ACT_BACK  0x20u
#define ACT_BKSP  0x40u
static unsigned s_act;
static char     s_typed[16];
static int      s_typed_n;
static bool     s_eat_chars;

static char   s_msg[64];
static double s_msg_until;

void cheat_ui_open(void) {
    s_open      = true;
    s_cursor    = 0;
    s_query[0]  = '\0';
    s_folded[0] = '\0';
    s_act       = 0;
    s_typed_n   = 0;
    s_msg_until = 0.0;
    // The backtick that opened this also arrives as a character (F-80's
    // neighbour: the crafting book had the same problem with C).
    s_eat_chars = true;
}

void cheat_ui_close(void) {
    s_open = false;
}

bool cheat_ui_active(void) {
    return s_open;
}

void cheat_ui_event(bsp_input_event_t const* ev) {
    if (!s_open || ev == NULL) return;
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
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
        char const c = ev->args_keyboard.ascii;
        // The backtick is the console's own key and never its content.
        if (c >= 32 && c < 127 && c != '`' && s_typed_n < (int)sizeof(s_typed)) s_typed[s_typed_n++] = c;
    }
}

// EVERY item in the game, matched on its stable name. Not the ones the
// player has seen, and not translated: this is the cheat console.
static void filter(void) {
    s_list_n = 0;
    for (uint16_t id = 1; id < ITEM_COUNT && s_list_n < CHEAT_MAX; id++) {
        if (id == BLK_AIR || id == BLK_BARRIER) continue;
        char const* const name = item_def(id).name;
        if (name == NULL || name[0] == '\0') continue;
        if (!fold_match(name, s_folded)) continue;
        s_list[s_list_n++] = id;
    }
}

void cheat_ui_update(inventory_t* inv) {
    if (!s_open || inv == NULL) return;

    if (s_eat_chars) {
        s_typed_n   = 0;
        s_eat_chars = false;
    }

    int len = (int)strlen(s_query);
    for (int i = 0; i < s_typed_n && len + 1 < (int)sizeof(s_query); i++) {
        s_query[len++] = s_typed[i];
        s_query[len]   = '\0';
    }
    if ((s_act & ACT_BKSP) && len > 0) s_query[--len] = '\0';
    if (s_typed_n > 0 || (s_act & ACT_BKSP)) {
        fold_text(s_query, s_folded, sizeof(s_folded));
        s_cursor = 0;
    }
    s_typed_n = 0;

    filter();

    if (s_act & ACT_BACK) {
        s_open = false;
        s_act  = 0;
        return;
    }
    if (s_act & ACT_LEFT) s_amount--;
    if (s_act & ACT_RIGHT) s_amount++;
    if (s_amount < 0) s_amount = 0;
    if (s_amount >= AMOUNT_N) s_amount = AMOUNT_N - 1;

    if (s_list_n > 0) {
        if (s_act & ACT_UP) s_cursor--;
        if (s_act & ACT_DOWN) s_cursor++;
        if (s_cursor < 0) s_cursor = 0;
        if (s_cursor >= s_list_n) s_cursor = s_list_n - 1;

        if (s_act & ACT_OK) {
            uint16_t const id  = s_list[s_cursor];
            int const      cap = item_def(id).stack_max;
            int            n   = AMOUNTS[s_amount];
            if (n > cap) n = cap;
            int const left = inv_add(inv, id, n, 0);
            snprintf(s_msg, sizeof(s_msg), "gave %d %s%s", n - left, item_def(id).name,
                     left > 0 ? "  (pack full)" : "");
            s_msg_until = showtime_now() + MSG_SECONDS;
            sfx_play(left < n ? SFX_PICKUP : SFX_DENY);
        }
    } else {
        s_cursor = 0;
    }
    s_act = 0;
}

void cheat_ui_draw(pax_buf_t* fb) {
    if (!s_open) return;

    se_menu_row_t rows[CHEAT_MAX];
    static char   vals[CHEAT_MAX][16];
    int           n = 0;

    for (int i = 0; i < s_list_n; i++) {
        uint16_t const id = s_list[i];
        snprintf(vals[n], sizeof(vals[n]), "%s%u", item_is_block(id) ? "block " : "item ", (unsigned)id);
        memset(&rows[n], 0, sizeof(rows[n]));
        rows[n].label = item_def(id).name;
        rows[n].kind  = SE_MENU_VAL_TEXT;
        rows[n].value = vals[n];
        n++;
    }
    if (n == 0) {
        memset(&rows[0], 0, sizeof(rows[0]));
        rows[0].label = "nothing matches";
        n             = 1;
    }

    char sub[FOLD_MAX + 48];
    snprintf(sub, sizeof(sub), "/%s_      give: %d", s_query, AMOUNTS[s_amount]);

    char foot[96];
    if (showtime_now() < s_msg_until) {
        snprintf(foot, sizeof(foot), "%s", s_msg);
    } else {
        snprintf(foot, sizeof(foot), "type to search   left/right sets how many   enter gives   esc closes");
    }

    se_menu_def_t const def = {
        .title        = "cheat console",
        .subtitle     = sub,
        .rows         = rows,
        .row_count    = n,
        .hint         = foot,
        .title_h      = 30.0f,
        .row_h        = 32.0f,
        .value_dx     = 380.0f,
        .panel_w      = 0.88f,
        .panel_h      = 0.94f,
        .visible_rows = VISIBLE,
    };
    se_menu_t const m = {.def = &def, .cursor = s_list_n > 0 ? s_cursor : n};
    se_menu_draw(&m, fb);
}
