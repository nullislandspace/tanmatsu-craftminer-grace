// =====================================================================
//  SynthMiner  --  the furnace screen (see furnace_ui.h)
// =====================================================================

#include "ui/furnace_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "game/furnace.h"
#include "i18n/i18n.h"
#include "items/items.h"
#include "items/recipes.h"
#include "se_ui.h"
#include "ui/amount_ui.h"
#include "testkit/showtime.h"

#define ROW_INPUT  0
#define ROW_FUEL   1
#define ROW_OUTPUT 2
#define ROW_COUNT  3

#define MSG_SECONDS 1.8

static bool    s_open;
static int32_t s_x, s_y, s_z;
static int     s_cursor;

// The picker, over the player's own slots. -1 when it is not up;
// otherwise the furnace row it is filling.
static int s_pick_for = -1;
static int s_pick_cursor;
static int s_pick[INV_SLOTS];
static int s_pick_n;

#define ACT_UP   0x01u
#define ACT_DOWN 0x02u
#define ACT_OK   0x04u
#define ACT_BACK 0x08u
static unsigned s_act;

static char   s_msg[64];
static double s_msg_until;

// A put waiting on "how many?". The slot, not a pointer: see chest_ui.c.
static bool s_asking;
static int  s_ask_slot;   // the player's slot the stack comes from
static int  s_ask_into;   // BE_FURNACE_INPUT or BE_FURNACE_FUEL

static blockent_t* furnace(void) {
    blockent_t* b = blockent_at(s_x, s_y, s_z);
    return (b != NULL && b->kind == BE_FURNACE) ? b : NULL;
}

bool furnace_ui_open(int32_t x, int32_t y, int32_t z) {
    blockent_t* b = blockent_at(x, y, z);
    if (b == NULL || b->kind != BE_FURNACE) return false;
    s_open      = true;
    s_x         = x;
    s_y         = y;
    s_z         = z;
    s_cursor    = ROW_INPUT;
    s_pick_for  = -1;
    s_act       = 0;
    s_asking    = false;
    s_msg_until = 0.0;
    return true;
}

void furnace_ui_close(void) {
    s_open     = false;
    s_pick_for = -1;
}

bool furnace_ui_active(void) {
    return s_open;
}

void furnace_ui_event(bsp_input_event_t const* ev) {
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
        switch (sc) {
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN: s_act |= ACT_DOWN; break;
            default: break;
        }
    }
}

// Which of the player's stacks could go in the slot being filled.
static void build_pick(inventory_t const* inv) {
    s_pick_n = 0;
    for (int i = 0; i < INV_SLOTS; i++) {
        inv_slot_t const* s = &inv->slot[i];
        if (s->item == 0 || s->count == 0) continue;
        bool const ok = (s_pick_for == ROW_FUEL) ? furnace_is_fuel(s->item) : furnace_smelts_to(s->item) != 0;
        if (ok) s_pick[s_pick_n++] = i;
    }
}

// Move a whole stack from the player into a furnace slot. What is
// already there comes BACK, so choosing the wrong thing is undone by
// choosing the right one rather than by breaking the furnace.
static void put_in(inventory_t* inv, blockent_t* be, int be_slot, int inv_slot, int want) {
    inv_slot_t* src = &inv->slot[inv_slot];
    inv_slot_t* dst = &be->slot[be_slot];
    if (src->item == 0 || want <= 0) return;
    if (want > src->count) want = src->count;

    if (dst->item == src->item && dst->wear == src->wear) {
        int const cap  = item_def(dst->item).stack_max;
        int const room = cap - dst->count;
        int const take = want < room ? want : room;
        dst->count     = (uint8_t)(dst->count + take);
        src->count     = (uint8_t)(src->count - take);
        if (src->count == 0) {
            src->item = 0;
            src->wear = 0;
        }
        return;
    }

    // A different thing was in the slot: it comes out, and only the
    // asked-for part of the new stack goes in.
    inv_slot_t const was = *dst;
    *dst                 = *src;
    dst->count           = (uint8_t)want;
    src->count           = (uint8_t)(src->count - want);
    if (src->count == 0) {
        src->item = 0;
        src->wear = 0;
    }
    if (was.item != 0 && was.count > 0) {
        // Back into the pack -- and if it will not fit, back into the
        // slot it came from, which is now free. Nothing is ever lost.
        int const left = inv_add(inv, was.item, was.count, was.wear);
        if (left > 0) {
            src->item  = was.item;
            src->count = (uint8_t)left;
            src->wear  = was.wear;
        }
    }
}

void furnace_ui_update(inventory_t* inv, uint32_t now) {
    if (!s_open || inv == NULL) return;

    blockent_t* be = furnace();
    if (be == NULL) {  // broken while it was open
        furnace_ui_close();
        return;
    }
    // Everything the furnace has done since it was last looked at. It
    // does not tick, so this IS the furnace running (game/furnace.h).
    furnace_catch_up(be, now);

    if (s_asking) {
        int const want = amount_update();
        if (want == AMOUNT_PENDING) return;
        s_asking = false;
        s_act    = 0;
        if (want > 0) {
            put_in(inv, be, s_ask_into, s_ask_slot, want);
            furnace_catch_up(be, now);
            blockent_touch(be);
            sfx_play(SFX_PLACE);
        }
        s_pick_for = -1;
        return;
    }

    // --- The picker ---------------------------------------------------
    if (s_pick_for >= 0) {
        build_pick(inv);
        if (s_act & ACT_BACK) {
            s_pick_for = -1;
            s_act      = 0;
            return;
        }
        if (s_pick_n > 0) {
            if (s_act & ACT_UP) s_pick_cursor--;
            if (s_act & ACT_DOWN) s_pick_cursor++;
            if (s_pick_cursor < 0) s_pick_cursor = 0;
            if (s_pick_cursor >= s_pick_n) s_pick_cursor = s_pick_n - 1;
            if (s_act & ACT_OK) {
                inv_slot_t const* src  = &inv->slot[s_pick[s_pick_cursor]];
                int const         into = s_pick_for == ROW_FUEL ? BE_FURNACE_FUEL : BE_FURNACE_INPUT;
                if (src->count > 1) {
                    s_asking   = true;
                    s_ask_slot = s_pick[s_pick_cursor];
                    s_ask_into = into;
                    amount_open(src->item, src->count);
                    s_act = 0;
                    return;
                }
                put_in(inv, be, into, s_pick[s_pick_cursor], 1);
                furnace_catch_up(be, now);  // it may start this instant
                blockent_touch(be);         // or the card never hears about it
                sfx_play(SFX_PLACE);
                s_pick_for = -1;
            }
        } else {
            s_pick_cursor = 0;
        }
        s_act = 0;
        return;
    }

    // --- The three rows -----------------------------------------------
    if (s_act & ACT_BACK) {
        furnace_ui_close();
        s_act = 0;
        return;
    }
    if (s_act & ACT_UP) s_cursor--;
    if (s_act & ACT_DOWN) s_cursor++;
    if (s_cursor < 0) s_cursor = 0;
    if (s_cursor >= ROW_COUNT) s_cursor = ROW_COUNT - 1;

    if (s_act & ACT_OK) {
        if (s_cursor == ROW_OUTPUT) {
            inv_slot_t* o = &be->slot[BE_FURNACE_OUTPUT];
            if (o->item == 0 || o->count == 0) {
                sfx_play(SFX_DENY);
            } else {
                uint16_t const what = o->item;
                int const      had  = o->count;
                int const      left = inv_add(inv, o->item, o->count, o->wear);
                int const      took = had - left;
                if (took <= 0) {
                    sfx_play(SFX_DENY);
                } else {
                    o->count = (uint8_t)left;
                    if (o->count == 0) {
                        o->item = 0;
                        o->wear = 0;
                    }
                    blockent_touch(be);
                    i18n_fmt(s_msg, sizeof(s_msg), SM_STR_FURNACE_TOOK, took, T(item_label(what)));
                    s_msg_until = showtime_now() + MSG_SECONDS;
                    sfx_play(SFX_PICKUP);
                }
            }
        } else {
            s_pick_for    = s_cursor;
            s_pick_cursor = 0;
            build_pick(inv);
        }
    }
    s_act = 0;
}

// "3 Coal", or "- empty -".
static void slot_text(inv_slot_t const* s, char* out, size_t cap) {
    if (s->item == 0 || s->count == 0) {
        snprintf(out, cap, "%s", T(SM_STR_FURNACE_EMPTY));
    } else {
        i18n_fmt(out, cap, SM_STR_FURNACE_SLOT, (int)s->count, T(item_label(s->item)));
    }
}

static void draw_picker(pax_buf_t* fb, inventory_t const* inv) {
    se_menu_row_t rows[INV_SLOTS];
    static char   labels[INV_SLOTS][64];
    bool const    fuel = s_pick_for == ROW_FUEL;

    int n = 0;
    for (int i = 0; i < s_pick_n; i++) {
        inv_slot_t const* s = &inv->slot[s_pick[i]];
        i18n_fmt(labels[n], sizeof(labels[n]), SM_STR_FURNACE_SLOT, (int)s->count, T(item_label(s->item)));
        memset(&rows[n], 0, sizeof(rows[n]));
        rows[n].label = labels[n];
        rows[n].kind  = SE_MENU_VAL_NONE;
        n++;
    }
    if (n == 0) {
        memset(&rows[0], 0, sizeof(rows[0]));
        rows[0].label = T(fuel ? SM_STR_FURNACE_PICK_NONE_FUEL : SM_STR_FURNACE_PICK_NONE_INPUT);
        n             = 1;
    }

    // WHAT THE CURSOR'S STACK WOULD DO, in the footer rather than in a
    // value column beside the name. A stack's name is already most of
    // an 800 px row at 28 px, and "makes Wooden pickaxe" beside it does
    // not fit in any language -- the footer is 14 px and has the whole
    // panel (worldcheck's "does every line fit where it is drawn").
    char foot[128];
    if (s_pick_n > 0) {
        inv_slot_t const* s = &inv->slot[s_pick[s_pick_cursor]];
        if (fuel) {
            // How many items this stack would see through, which is the
            // only number that makes one fuel comparable with another.
            int const per = furnace_fuel_ticks(s->item) / FURNACE_COOK_TICKS;
            i18n_fmt(foot, sizeof(foot), SM_STR_FURNACE_BURNS, per * (int)s->count);
        } else {
            i18n_fmt(foot, sizeof(foot), SM_STR_FURNACE_BECOMES, T(item_label(furnace_smelts_to(s->item))));
        }
    } else {
        snprintf(foot, sizeof(foot), "%s", T(SM_STR_FURNACE_PICK_HINT));
    }

    se_menu_def_t const def = {
        .title        = T(fuel ? SM_STR_FURNACE_PICK_FUEL : SM_STR_FURNACE_PICK_INPUT),
        .rows         = rows,
        .row_count    = n,
        .hint         = foot,
        .title_h      = 32.0f,
        .row_h        = 34.0f,
        .value_dx     = 0.0f,
        .panel_w      = 0.88f,
        .panel_h      = 0.92f,
        .visible_rows = 8,
    };
    se_menu_t const m = {.def = &def, .cursor = s_pick_n > 0 ? s_pick_cursor : n};
    se_menu_draw(&m, fb);
}

void furnace_ui_draw(pax_buf_t* fb, inventory_t const* inv) {
    if (!s_open || inv == NULL) return;
    blockent_t const* be = furnace();
    if (be == NULL) return;

    if (s_pick_for >= 0 || s_asking) {
        draw_picker(fb, inv);
        // Taking the OUTPUT never asks -- there is no reason to leave
        // half a smelt in the furnace (the user's rule).
        if (s_asking) amount_draw(fb);
        return;
    }

    static char  vals[ROW_COUNT][48];
    se_menu_row_t rows[ROW_COUNT];
    sm_str_t const names[ROW_COUNT] = {SM_STR_FURNACE_INPUT, SM_STR_FURNACE_FUEL, SM_STR_FURNACE_OUTPUT};
    int const      slots[ROW_COUNT] = {BE_FURNACE_INPUT, BE_FURNACE_FUEL, BE_FURNACE_OUTPUT};

    for (int i = 0; i < ROW_COUNT; i++) {
        slot_text(&be->slot[slots[i]], vals[i], sizeof(vals[i]));
        memset(&rows[i], 0, sizeof(rows[i]));
        rows[i].label = T(names[i]);
        rows[i].kind  = SE_MENU_VAL_TEXT;
        rows[i].value = vals[i];
    }

    // What it is doing, or the reason it is not. A furnace that sits
    // there doing nothing has to say why, or the player is left poking
    // at three slots guessing which one is wrong.
    char footer[96];
    if (showtime_now() < s_msg_until) {
        snprintf(footer, sizeof(footer), "%s", s_msg);
    } else {
        switch (furnace_idle_reason(be)) {
            case FURNACE_IDLE_NO_INPUT: snprintf(footer, sizeof(footer), "%s", T(SM_STR_FURNACE_NO_INPUT)); break;
            case FURNACE_IDLE_NO_FUEL: snprintf(footer, sizeof(footer), "%s", T(SM_STR_FURNACE_NO_FUEL)); break;
            case FURNACE_IDLE_FULL: snprintf(footer, sizeof(footer), "%s", T(SM_STR_FURNACE_FULL)); break;
            default: i18n_fmt(footer, sizeof(footer), SM_STR_FURNACE_SMELTING, furnace_progress_pct(be)); break;
        }
    }

    se_menu_def_t const def = {
        .title     = T(SM_STR_FURNACE_TITLE),
        .subtitle  = footer,
        .rows      = rows,
        .row_count = ROW_COUNT,
        .hint      = T(SM_STR_FURNACE_HINT),
        .title_h   = 32.0f,
        .row_h     = 40.0f,
        .value_dx  = 240.0f,
        .panel_w   = 0.94f,
        .panel_h   = 0.66f,
    };
    se_menu_t const m = {.def = &def, .cursor = s_cursor};
    se_menu_draw(&m, fb);
}
