// =====================================================================
//  CraftMiner  --  chests, and the trashcan (see chest_ui.h)
// =====================================================================

#include "ui/chest_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "game/hud.h"
#include "i18n/i18n.h"
#include "items/items.h"
#include "se_direct565.h"
#include "se_text.h"
#include "synthengine3d.h"
#include "testkit/showtime.h"
#include "world/blockent.h"

#define GAP         4
#define MSG_SECONDS 2.2

static bool    s_open;
static int32_t s_x, s_y, s_z;
static bool    s_on_chest;  // which grid has the arrow keys
static int     s_cur_chest, s_cur_inv;

#define ACT_UP    0x01u
#define ACT_DOWN  0x02u
#define ACT_LEFT  0x04u
#define ACT_RIGHT 0x08u
#define ACT_OK    0x10u
#define ACT_BACK  0x20u
#define ACT_TAB   0x40u
static unsigned s_act;

static char   s_msg[64];
static double s_msg_until;

static blockent_t* container(void) {
    blockent_t* b = blockent_at(s_x, s_y, s_z);
    if (b == NULL) return NULL;
    return (b->kind == BE_CHEST || b->kind == BE_TRASH) ? b : NULL;
}

bool chest_ui_open(int32_t x, int32_t y, int32_t z) {
    blockent_t* b = blockent_at(x, y, z);
    if (b == NULL || (b->kind != BE_CHEST && b->kind != BE_TRASH)) return false;
    s_open      = true;
    s_x         = x;
    s_y         = y;
    s_z         = z;
    s_on_chest  = true;
    s_cur_chest = 0;
    s_cur_inv   = 0;
    s_act       = 0;
    s_msg_until = 0.0;
    return true;
}

void chest_ui_close(void) {
    s_open = false;
}

bool chest_ui_active(void) {
    return s_open;
}

void chest_ui_event(bsp_input_event_t const* ev) {
    if (!s_open || ev == NULL) return;
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: s_act |= ACT_OK; break;
            case BSP_INPUT_NAVIGATION_KEY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_NAVIGATION_KEY_DOWN: s_act |= ACT_DOWN; break;
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
            case BSP_INPUT_SCANCODE_TAB: s_act |= ACT_TAB; break;
            default: break;
        }
    }
}

static void move_cursor(int* cur, int n, int cols) {
    int x = *cur % cols, y = *cur / cols;
    int const rows = (n + cols - 1) / cols;
    if (s_act & ACT_LEFT) x--;
    if (s_act & ACT_RIGHT) x++;
    if (s_act & ACT_UP) y--;
    if (s_act & ACT_DOWN) y++;
    if (x < 0) x = 0;
    if (x >= cols) x = cols - 1;
    if (y < 0) y = 0;
    if (y >= rows) y = rows - 1;
    int const at = y * cols + x;
    *cur         = at < n ? at : n - 1;
}

// Move one stack from `from` to the first place it fits in `to`, which
// may be a partial stack of the same thing. Returns how many moved.
static int transfer(inv_slot_t* from, inv_slot_t* to, int to_n) {
    if (from->item == 0 || from->count == 0) return 0;
    int const cap   = item_def(from->item).stack_max;
    int       moved = 0;

    if (cap > 1) {
        for (int i = 0; i < to_n && from->count > 0; i++) {
            if (to[i].item != from->item || to[i].wear != from->wear || to[i].count >= cap) continue;
            int const room = cap - to[i].count;
            int const take = from->count < room ? from->count : room;
            to[i].count    = (uint8_t)(to[i].count + take);
            from->count    = (uint8_t)(from->count - take);
            moved += take;
        }
    }
    for (int i = 0; i < to_n && from->count > 0; i++) {
        if (to[i].item != 0) continue;
        to[i]       = *from;
        moved += from->count;
        from->count = 0;
    }
    if (from->count == 0) {
        from->item = 0;
        from->wear = 0;
    }
    return moved;
}

void chest_ui_update(inventory_t* inv, uint32_t now) {
    if (!s_open || inv == NULL) return;
    blockent_t* be = container();
    if (be == NULL) {  // broken while it was open
        chest_ui_close();
        return;
    }

    // A chest's stamp is only ever "now"; a trashcan's is what its
    // contents are measured against (world/blockent.h).
    if (be->kind != BE_TRASH) be->stamp = now;
    int const gone = blockent_rot_trash(be, now);
    if (gone > 0) {
        i18n_fmt(s_msg, sizeof(s_msg), CM_STR_CHEST_TRASH_GONE, gone);
        s_msg_until = showtime_now() + MSG_SECONDS;
        blockent_touch(be);
    }

    if (s_act & ACT_BACK) {
        chest_ui_close();
        s_act = 0;
        return;
    }
    if (s_act & ACT_TAB) s_on_chest = !s_on_chest;

    if (s_on_chest) {
        move_cursor(&s_cur_chest, CHEST_SLOTS, CHEST_COLS);
    } else {
        move_cursor(&s_cur_inv, INV_SLOTS, INV_HOTBAR);
    }

    if (s_act & ACT_OK) {
        int moved;
        if (s_on_chest) {
            moved = transfer(&be->slot[s_cur_chest], inv->slot, INV_SLOTS);
        } else {
            moved = transfer(&inv->slot[s_cur_inv], be->slot, CHEST_SLOTS);
            // Fresh rubbish resets the bin's clock: ten minutes from
            // the last thing you threw away, not from the first.
            if (moved > 0 && be->kind == BE_TRASH) be->stamp = now;
        }
        if (moved > 0) {
            blockent_touch(be);
            sfx_play(SFX_PICKUP);
        } else {
            snprintf(s_msg, sizeof(s_msg), "%s", T(CM_STR_CHEST_NO_ROOM));
            s_msg_until = showtime_now() + MSG_SECONDS;
            sfx_play(SFX_DENY);
        }
    }
    s_act = 0;
}

void chest_ui_draw(pax_buf_t* fb, inventory_t const* inv) {
    if (!s_open || inv == NULL) return;
    blockent_t const* be = container();
    if (be == NULL) return;

    int const sw = CHEST_SLOT_W;
    int const gw = CHEST_COLS * sw + (CHEST_COLS - 1) * GAP;
    int const gh = CHEST_ROWS * sw + (CHEST_ROWS - 1) * GAP;
    int const x0 = ((int)DISPLAY_LOG_W - (2 * gw + 40)) / 2;
    int const x1 = x0 + gw + 40;
    int const y0 = 150;

    direct_565_dim_rect((uint16_t*)pax_buf_get_pixels_rw(fb), fb->reverse_endianness, 0, 0, (int)DISPLAY_LOG_W,
                        (int)DISPLAY_LOG_H);

    bool const trash = be->kind == BE_TRASH;
    char const* const title = T(trash ? CM_STR_CHEST_TITLE_TRASH : CM_STR_CHEST_TITLE);
    pax_vec2f const   tsz   = rendertext_size(NULL, 30.0f, title);
    rendertext_draw(fb, 0xFFFFFFFFu, NULL, 30.0f, ((float)DISPLAY_LOG_W - tsz.x) * 0.5f, 42.0f, title);

    rendertext_draw(fb, 0xFFB0B0B8u, NULL, 18.0f, (float)x0, (float)(y0 - 24), title);
    rendertext_draw(fb, 0xFFB0B0B8u, NULL, 18.0f, (float)x1, (float)(y0 - 24), T(CM_STR_CHEST_YOURS));

    hud_slot_grid(fb, x0, y0, be->slot, CHEST_SLOTS, CHEST_COLS, sw, s_on_chest ? s_cur_chest : -1, s_on_chest);
    hud_slot_grid(fb, x1, y0, inv->slot, INV_SLOTS, INV_HOTBAR, sw, s_on_chest ? -1 : s_cur_inv, !s_on_chest);

    // What just happened, or -- for a bin -- the rule, which a player
    // has to be told before they use it rather than after.
    char foot[128];
    if (showtime_now() < s_msg_until) {
        snprintf(foot, sizeof(foot), "%s", s_msg);
    } else if (trash) {
        i18n_fmt(foot, sizeof(foot), CM_STR_CHEST_TRASH_WARN, (int)BE_TRASH_MINUTES);
    } else {
        foot[0] = '\0';
    }
    if (foot[0] != '\0') {
        pax_vec2f const fsz = rendertext_size(NULL, 16.0f, foot);
        rendertext_draw(fb, 0xFFD0B060u, NULL, 16.0f, ((float)DISPLAY_LOG_W - fsz.x) * 0.5f,
                        (float)(y0 + gh + 14), foot);
    }

    char const* const hint = T(CM_STR_CHEST_HINT);
    pax_vec2f const   hsz  = rendertext_size(NULL, 15.0f, hint);
    rendertext_draw(fb, 0xFF9090A0u, NULL, 15.0f, ((float)DISPLAY_LOG_W - hsz.x) * 0.5f,
                    (float)DISPLAY_LOG_H - 34.0f, hint);
}
