// =====================================================================
//  SynthMiner  --  keybind value rendering (see keybind_ui.h)
//  Ported from tanmatsu-synthracer-grace, main/keybind_ui.c. Changes:
//  it draws on the `fb` se_ui hands it rather than a global, and the
//  text labels come from input_key_name() (game/input.h), which also
//  names the cursor keys -- synthracer never bound those, SynthMiner
//  ships them as the look keys.
// =====================================================================

#include "ui/keybind_ui.h"

#include <stdint.h>
#include <stdio.h>

#include "bsp/input.h"
#include "game/input.h"
#include "se_text.h"
#include "ui/icons.h"

// Map a scancode to a key icon, or -1 if none exists. icons.c loads
// PNGs for Esc and F1..F6; those keys render as their icon. Every other
// key falls back to text, and so does an icon that failed to load.
static int scancode_icon(uint16_t sc) {
    switch (sc) {
        case BSP_INPUT_SCANCODE_ESC: return ICON_ESC;
        case BSP_INPUT_SCANCODE_F1: return ICON_F1;
        case BSP_INPUT_SCANCODE_F2: return ICON_F2;
        case BSP_INPUT_SCANCODE_F3: return ICON_F3;
        case BSP_INPUT_SCANCODE_F4: return ICON_F4;
        case BSP_INPUT_SCANCODE_F5: return ICON_F5;
        case BSP_INPUT_SCANCODE_F6: return ICON_F6;
        default: return -1;
    }
}

// Draw the value side of a keybind row at (x, y): the function-key
// icon when one exists and loaded, otherwise the text label.
static void draw_keybind_value(pax_buf_t* fb, float x, float y, float text_h, pax_col_t col, uint16_t sc) {
    int const icon = scancode_icon(sc);
    if (icon >= 0 && icons_width((icon_key_t)icon) > 0) {
        int const   iw = icons_width((icon_key_t)icon);
        int const   ih = icons_height((icon_key_t)icon);
        float const iy = y + text_h * 0.5f - (float)ih * 0.5f;
        // The key-hint PNGs are black glyphs on a transparent
        // background -- invisible on the dim menu panel. Lay down a
        // white tile first so the icon reads like a physical keycap.
        pax_simple_rect(fb, 0xFFFFFFFFu, x, iy, (float)iw, (float)ih);
        icons_blit(fb, (icon_key_t)icon, x, iy);
        return;
    }
    char buf[24];
    rendertext_draw(fb, col, NULL, text_h, x, y, input_key_name(sc, buf, sizeof(buf)));
}

void controls_keybind_draw(pax_buf_t* fb, float x, float y, float h, pax_col_t col, void* ctx) {
    draw_keybind_value(fb, x, y, h, col, (uint16_t)(uintptr_t)ctx);
}
