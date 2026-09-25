// =====================================================================
//  SynthMiner  --  the disassembly bench (see bench_ui.h)
// =====================================================================

#include "ui/bench_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "i18n/i18n.h"
#include "items/items.h"
#include "items/recipes.h"
#include "se_ui.h"
#include "testkit/showtime.h"

#define MSG_SECONDS 2.0

static bool s_open;
static int  s_cursor;

// The recipe behind each listed item, so the list and the work cannot
// disagree about what is being taken apart.
static uint16_t s_item[INV_SLOTS];
static int      s_n;

#define ACT_UP   0x01u
#define ACT_DOWN 0x02u
#define ACT_OK   0x04u
#define ACT_BACK 0x08u
static unsigned s_act;

static char   s_msg[64];
static double s_msg_until;

void bench_ui_open(void) {
    s_open      = true;
    s_cursor    = 0;
    s_act       = 0;
    s_msg_until = 0.0;
}

void bench_ui_close(void) {
    s_open = false;
}

bool bench_ui_active(void) {
    return s_open;
}

void bench_ui_event(bsp_input_event_t const* ev) {
    if (!s_open || ev == NULL) return;
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: s_act |= ACT_OK; break;
            case BSP_INPUT_NAVIGATION_KEY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_NAVIGATION_KEY_DOWN: s_act |= ACT_DOWN; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if ((sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return;
        if (sc == BSP_INPUT_SCANCODE_ESCAPED_GREY_UP) s_act |= ACT_UP;
        if (sc == BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN) s_act |= ACT_DOWN;
    }
}

// The recipe that made `item`, if the bench is allowed to undo it.
static recipe_t const* undo_of(uint16_t item) {
    for (int i = 0; i < recipe_count(); i++) {
        recipe_t const* r = recipe_at(i);
        if (r->out == item && (r->flags & RF_REVERSIBLE) != 0) return r;
    }
    return NULL;
}

// What is carried that comes apart, each item once however many stacks
// of it there are.
static void build(inventory_t const* inv) {
    s_n = 0;
    for (int i = 0; i < INV_SLOTS; i++) {
        uint16_t const it = inv->slot[i].item;
        if (it == 0 || inv->slot[i].count == 0) continue;
        if (undo_of(it) == NULL) continue;
        bool seen = false;
        for (int k = 0; k < s_n; k++) {
            if (s_item[k] == it) seen = true;
        }
        if (!seen) s_item[s_n++] = it;
    }
}

void bench_ui_update(inventory_t* inv) {
    if (!s_open || inv == NULL) return;
    build(inv);

    if (s_act & ACT_BACK) {
        bench_ui_close();
        s_act = 0;
        return;
    }
    if (s_n > 0) {
        if (s_act & ACT_UP) s_cursor--;
        if (s_act & ACT_DOWN) s_cursor++;
        if (s_cursor < 0) s_cursor = 0;
        if (s_cursor >= s_n) s_cursor = s_n - 1;

        if (s_act & ACT_OK) {
            uint16_t const  it = s_item[s_cursor];
            recipe_t const* r  = undo_of(it);
            // ALL OR NOTHING, like every other craft: the item only
            // goes if everything it becomes has somewhere to land.
            inventory_t const before = *inv;
            bool              ok     = r != NULL && inv_take(inv, it, r->out_n);
            for (int i = 0; ok && i < r->n_in; i++) {
                if (inv_add(inv, r->in[i].item, r->in[i].count, 0) != 0) ok = false;
            }
            if (ok) {
                i18n_fmt(s_msg, sizeof(s_msg), SM_STR_BENCH_DONE, (int)r->out_n, T(item_label(it)));
                sfx_play(SFX_CRAFT);
            } else {
                *inv = before;
                snprintf(s_msg, sizeof(s_msg), "%s", T(SM_STR_CRAFT_FULL));
                sfx_play(SFX_DENY);
            }
            s_msg_until = showtime_now() + MSG_SECONDS;
            build(inv);
            if (s_cursor >= s_n) s_cursor = s_n > 0 ? s_n - 1 : 0;
        }
    } else {
        s_cursor = 0;
    }
    s_act = 0;
}

void bench_ui_draw(pax_buf_t* fb, inventory_t const* inv) {
    if (!s_open || inv == NULL) return;

    se_menu_row_t rows[INV_SLOTS];
    static char   labels[INV_SLOTS][64];
    static char   vals[INV_SLOTS][24];

    int n = 0;
    for (int i = 0; i < s_n; i++) {
        snprintf(labels[n], sizeof(labels[n]), "%s", T(item_label(s_item[i])));
        snprintf(vals[n], sizeof(vals[n]), "%d", inv_count(inv, s_item[i]));
        memset(&rows[n], 0, sizeof(rows[n]));
        rows[n].label = labels[n];
        rows[n].kind  = SE_MENU_VAL_TEXT;
        rows[n].value = vals[n];
        n++;
    }
    if (n == 0) {
        memset(&rows[0], 0, sizeof(rows[0]));
        rows[0].label = T(SM_STR_BENCH_EMPTY);
        n             = 1;
    }

    // What the cursor's item gives back, in the footer -- the same
    // place the crafting book and the furnace picker put this, and for
    // the same reason: a list of ingredients does not fit in a column.
    char foot[160];
    if (showtime_now() < s_msg_until) {
        snprintf(foot, sizeof(foot), "%s", s_msg);
    } else if (s_n > 0) {
        recipe_t const* r = undo_of(s_item[s_cursor]);
        char            parts[128];
        parts[0] = '\0';
        for (int i = 0; r != NULL && i < r->n_in; i++) {
            char one[48];
            snprintf(one, sizeof(one), "%d %s", (int)r->in[i].count, T(item_label(r->in[i].item)));
            if (parts[0] != '\0') strncat(parts, "   ", sizeof(parts) - strlen(parts) - 1);
            strncat(parts, one, sizeof(parts) - strlen(parts) - 1);
        }
        i18n_fmt(foot, sizeof(foot), SM_STR_BENCH_GIVES, parts);
    } else {
        snprintf(foot, sizeof(foot), "%s", T(SM_STR_BENCH_HINT));
    }

    se_menu_def_t const def = {
        .title        = T(SM_STR_BENCH_TITLE),
        .subtitle     = foot,
        .rows         = rows,
        .row_count    = n,
        .hint         = T(SM_STR_BENCH_HINT),
        .title_h      = 32.0f,
        .row_h        = 34.0f,
        .value_dx     = 380.0f,
        .panel_w      = 0.88f,
        .panel_h      = 0.92f,
        .visible_rows = 8,
    };
    se_menu_t const m = {.def = &def, .cursor = s_n > 0 ? s_cursor : n};
    se_menu_draw(&m, fb);
}
