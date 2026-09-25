// =====================================================================
//  SynthMiner  --  "how many?" (see amount_ui.h)
// =====================================================================

#include "ui/amount_ui.h"

#include <stdio.h>
#include <string.h>

#include "audio/sfx.h"
#include "i18n/i18n.h"
#include "items/items.h"
#include "se_direct565.h"
#include "se_text.h"
#include "synthengine3d.h"

static bool     s_open;
static uint16_t s_item;
static int      s_max, s_value;
// True once a digit has been typed: the first one REPLACES the default
// rather than appending to it, or asking for 7 out of 64 means typing
// backspace twice before you can start.
static bool s_typed;

#define ACT_LEFT  0x01u
#define ACT_RIGHT 0x02u
#define ACT_UP    0x04u
#define ACT_DOWN  0x08u
#define ACT_OK    0x10u
#define ACT_BACK  0x20u
#define ACT_BKSP  0x40u
static unsigned s_act;
static char     s_digits[8];
static int      s_digits_n;

void amount_open(uint16_t item, int max) {
    s_open     = true;
    s_item     = item;
    s_max      = max < 1 ? 1 : max;
    s_value    = s_max;  // everything, which is what it did before it asked
    s_typed    = false;
    s_act      = 0;
    s_digits_n = 0;
}

bool amount_active(void) {
    return s_open;
}

void amount_event(bsp_input_event_t const* ev) {
    if (!s_open || ev == NULL) return;
    if (ev->type == INPUT_EVENT_TYPE_NAVIGATION && ev->args_navigation.state) {
        switch (ev->args_navigation.key) {
            case BSP_INPUT_NAVIGATION_KEY_ESC: s_act |= ACT_BACK; break;
            case BSP_INPUT_NAVIGATION_KEY_RETURN: s_act |= ACT_OK; break;
            case BSP_INPUT_NAVIGATION_KEY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_NAVIGATION_KEY_RIGHT: s_act |= ACT_RIGHT; break;
            case BSP_INPUT_NAVIGATION_KEY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_NAVIGATION_KEY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_NAVIGATION_KEY_BACKSPACE: s_act |= ACT_BKSP; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_SCANCODE) {
        uint16_t const sc = ev->args_scancode.scancode;
        if ((sc & BSP_INPUT_SCANCODE_RELEASE_MODIFIER) != 0) return;
        switch (sc) {
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_LEFT: s_act |= ACT_LEFT; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_RIGHT: s_act |= ACT_RIGHT; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_UP: s_act |= ACT_UP; break;
            case BSP_INPUT_SCANCODE_ESCAPED_GREY_DOWN: s_act |= ACT_DOWN; break;
            case BSP_INPUT_SCANCODE_BACKSPACE: s_act |= ACT_BKSP; break;
            default: break;
        }
    } else if (ev->type == INPUT_EVENT_TYPE_KEYBOARD) {
        char const c = ev->args_keyboard.ascii;
        if (c >= '0' && c <= '9' && s_digits_n < (int)sizeof(s_digits)) s_digits[s_digits_n++] = c;
    }
}

static void clamp(void) {
    if (s_value < 1) s_value = 1;
    if (s_value > s_max) s_value = s_max;
}

int amount_update(void) {
    if (!s_open) return AMOUNT_CANCELLED;

    for (int i = 0; i < s_digits_n; i++) {
        int const d = s_digits[i] - '0';
        if (!s_typed) {
            s_value = d;
            s_typed = true;
        } else if (s_value <= 99999) {
            s_value = s_value * 10 + d;
        }
    }
    s_digits_n = 0;

    if (s_act & ACT_BKSP) {
        s_value /= 10;
        s_typed  = true;
    }
    if (s_act & ACT_LEFT) s_value--;
    if (s_act & ACT_RIGHT) s_value++;
    // A stack is 64, so ten at a time crosses it in seven presses.
    if (s_act & ACT_DOWN) s_value -= 10;
    if (s_act & ACT_UP) s_value += 10;
    if (s_act & (ACT_LEFT | ACT_RIGHT | ACT_UP | ACT_DOWN)) s_typed = true;

    // Typing is allowed to go out of range while it is being typed --
    // 6 on the way to 64 is below nothing -- and is pinned on the way
    // out, so a stray digit cannot move more than there is.
    if (s_act & ACT_OK) {
        clamp();
        int const v = s_value;
        s_open      = false;
        s_act       = 0;
        sfx_play(SFX_CLICK);
        return v;
    }
    if (s_act & ACT_BACK) {
        s_open = false;
        s_act  = 0;
        return AMOUNT_CANCELLED;
    }
    if (s_value < 0) s_value = 0;
    if (s_value > s_max) s_value = s_max;
    s_act = 0;
    return AMOUNT_PENDING;
}

void amount_draw(pax_buf_t* fb) {
    if (!s_open || fb == NULL) return;

    int const w = 640, h = 190;
    int const x = ((int)DISPLAY_LOG_W - w) / 2, y = ((int)DISPLAY_LOG_H - h) / 2;
    uint16_t* px = (uint16_t*)pax_buf_get_pixels_rw(fb);

    direct_565_dim_rect(px, fb->reverse_endianness, x, y, w, h);
    pax_draw_rect(fb, 0xFF606068u, (float)x, (float)y, (float)w, 2.0f);
    pax_draw_rect(fb, 0xFF606068u, (float)x, (float)(y + h - 2), (float)w, 2.0f);
    pax_draw_rect(fb, 0xFF606068u, (float)x, (float)y, 2.0f, (float)h);
    pax_draw_rect(fb, 0xFF606068u, (float)(x + w - 2), (float)y, 2.0f, (float)h);

    char head[96];
    i18n_fmt(head, sizeof(head), SM_STR_AMOUNT_TITLE, T(item_label(s_item)));
    rendertext_draw(fb, 0xFFFFFFFFu, NULL, 22.0f, (float)(x + 22), (float)(y + 18), head);

    // The number, big, because it is the thing being decided.
    char num[24];
    snprintf(num, sizeof(num), "%d / %d", s_value, s_max);
    pax_vec2f const nsz = rendertext_size(NULL, 34.0f, num);
    rendertext_draw(fb, 0xFFFFD040u, NULL, 34.0f, (float)x + ((float)w - nsz.x) * 0.5f, (float)(y + 56), num);

    // ... and the same number as a bar, for "about half".
    int const bx = x + 26, bw = w - 52, by = y + 108, bh = 16;
    pax_draw_rect(fb, 0xFF2A2A32u, (float)bx, (float)by, (float)bw, (float)bh);
    int const fill = s_max > 0 ? bw * s_value / s_max : 0;
    pax_draw_rect(fb, 0xFF6090D0u, (float)bx, (float)by, (float)fill, (float)bh);
    pax_draw_rect(fb, 0xFF505058u, (float)bx, (float)by, (float)bw, 1.0f);

    char const* const hint = T(SM_STR_AMOUNT_HINT);
    pax_vec2f const   hsz  = rendertext_size(NULL, 14.0f, hint);
    rendertext_draw(fb, 0xFF9090A0u, NULL, 14.0f, (float)x + ((float)w - hsz.x) * 0.5f, (float)(y + h - 34), hint);
}
