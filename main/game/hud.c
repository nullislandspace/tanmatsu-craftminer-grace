// =====================================================================
//  CraftMiner  --  what is drawn over the world (see hud.h)
// =====================================================================

#include "game/hud.h"

#include <math.h>

#include "math/mesh_render.h"
#include "world/light.h"

#include <stdio.h>

#include "items/item_entity.h"
#include "se_text.h"
#include "se_direct565.h"
#include "shapes/pax_misc.h"
#include "synthengine3d.h"
#include "world/chunk.h"
#include "world/chunk_render.h"

// Black, and drawn a hair outside the block's own faces so the two do
// not fight over the same depth. The engine already nudges an edge
// towards the camera (se_scene.c, SCENE_LINE_BIAS); this is the
// belt to that pair of braces, and it is what makes the outline read
// as a box round the block rather than as dashes on it.
// Rectangles go straight into the framebuffer, not through
// pax_draw_rect.
//
// MEASURED: the overlay cost **13.4 ms a frame** through PAX -- fifteen
// per cent of the frame, for about a hundred and twenty rectangles
// covering some fourteen thousand pixels. At the memory speeds this
// hardware actually has (F-40) those pixels are worth about 1.5 ms, so
// the rest was per-call overhead: a matrix, a clip, a shader dispatch
// and a function-pointer setter, per rectangle.
//
// se_direct565.h is the engine's own answer to that, and it is public
// for exactly this reason. A rectangle becomes one contiguous halfword
// run per column -- the display is rotated, so a vertical run is what
// is contiguous in memory.
static uint16_t* s_px;   // this frame's framebuffer, set by each entry point
static bool      s_rev;  // ... and its byte order

static void hud_begin(pax_buf_t* fb) {
    s_px  = (uint16_t*)pax_buf_get_pixels_rw(fb);
    s_rev = fb->reverse_endianness;
}

static void box(pax_buf_t* fb, int x, int y, int w, int h, uint32_t argb) {
    (void)fb;
    if (s_px == NULL || w <= 0 || h <= 0) return;
    uint16_t const packed = direct_565_pack(argb, s_rev);
    for (int i = x; i < x + w; i++) direct_565_vrun(s_px, i, y, y + h - 1, packed);
}

#define OUTLINE_ARGB 0xFF101010u
#define OUTLINE_GROW 0.005f

void hud_block_outline(int32_t bx, int32_t by, int32_t bz) {
    int32_t ox, oz;
    chunk_render_origin(&ox, &oz);

    // Into the scene's space. The subtraction is in integers, so it is
    // exact however far from the origin the player has walked -- which
    // is the whole point of the floating origin.
    float const x0 = (float)(bx - ox) - OUTLINE_GROW;
    float const y0 = (float)by - OUTLINE_GROW;
    float const z0 = (float)(bz - oz) - OUTLINE_GROW;
    float const x1 = x0 + 1.0f + 2.0f * OUTLINE_GROW;
    float const y1 = y0 + 1.0f + 2.0f * OUTLINE_GROW;
    float const z1 = z0 + 1.0f + 2.0f * OUTLINE_GROW;

    // The twelve edges, each once: from every corner, along each axis
    // it is at the low end of.
    for (int i = 0; i < 8; i++) {
        float const px = (i & 1) ? x1 : x0, py = (i & 2) ? y1 : y0, pz = (i & 4) ? z1 : z0;
        for (int bit = 1; bit < 8; bit <<= 1) {
            if (i & bit) continue;
            int const   j  = i | bit;
            float const qx = (j & 1) ? x1 : x0, qy = (j & 2) ? y1 : y0, qz = (j & 4) ? z1 : z0;
            scene_line(px, py, pz, qx, qy, qz, OUTLINE_ARGB);
        }
    }
}

// --- The crosshair --------------------------------------------------------

#define CROSS_ARM   9  // pixels from the centre, along each arm
#define CROSS_GAP   2  // ... left clear in the middle, so the target shows
#define CROSS_THICK 2

// Inverted, not painted. A white crosshair vanishes against snow and a
// black one against a cave mouth; the inverse of whatever is behind it
// is legible against everything, which is why Minecraft's does the
// same.
static void invert_px(pax_buf_t* fb, int x, int y) {
    (void)fb;
    if (s_px == NULL || x < 0 || y < 0 || x >= DISPLAY_LOG_W || y >= DISPLAY_LOG_H) return;
    // Straight on the packed halfword: inverting every bit of an RGB565
    // pixel inverts all three channels, whatever the byte order, so
    // this needs no unpack and no endian branch.
    uint16_t* const px = &s_px[direct_565_logical_index(x, y)];
    *px                = (uint16_t)~*px;
}

void hud_crosshair(pax_buf_t* fb) {
    if (fb == NULL) return;
    hud_begin(fb);
    // Where the camera's forward axis lands, from the engine's own
    // projection constants -- NOT the middle of the screen.
    int const cx = (int)RENDER_HALF_W;
    int const cy = (int)RENDER_HORIZON_Y;

    for (int t = 0; t < CROSS_THICK; t++) {
        for (int i = CROSS_GAP; i <= CROSS_ARM; i++) {
            invert_px(fb, cx - i, cy + t);
            invert_px(fb, cx + i, cy + t);
            invert_px(fb, cx + t, cy - i);
            invert_px(fb, cx + t, cy + i);
        }
    }
}

// --- The dropped items ----------------------------------------------------
//
// A small cube each, submitted like any other geometry and so depth-
// tested against the world. They spin, because a thing on the ground
// that does not move is a thing you walk past.

void hud_dropped_items(void) {
    int32_t ox, oz;
    chunk_render_origin(&ox, &oz);

    for (int i = 0; i < ITEM_ENTITY_MAX; i++) {
        item_entity_t const* e = item_entity_at(i);
        if (e == NULL || !e->alive) continue;

        float const      x = (float)(e->body.x - (double)ox);
        float const      y = (float)e->body.y;
        float const      z = (float)(e->body.z - (double)oz);
        float const      h = ITEM_ENTITY_SIZE * 0.5f;
        uint32_t const   c = item_def(e->item).argb;
        // Lit by the cell it lies in, like the ground under it: a drop
        // must not glow in the dark.
        uint32_t const lf = SE_TRI_LIGHT(mesh_light_level(world_light((int32_t)floor(e->body.x),
                                                                       (int32_t)floor(e->body.y + 0.1),
                                                                       (int32_t)floor(e->body.z))));

        // An axis-aligned box: six quads, two triangles each. Not spun
        // -- a rotation would have to be a function of the tick to stay
        // replayable, and the tick is not here. Flat colour, so this
        // costs the cheap rasteriser path.
        float const x0 = x - h, x1 = x + h, y0 = y, y1 = y + ITEM_ENTITY_SIZE, z0 = z - h, z1 = z + h;
        struct {
            float a[3], b[3], c[3], d[3];
        } const faces[6] = {
            {{x1, y0, z0}, {x1, y0, z1}, {x1, y1, z1}, {x1, y1, z0}},  // +x
            {{x0, y0, z1}, {x0, y0, z0}, {x0, y1, z0}, {x0, y1, z1}},  // -x
            {{x0, y1, z0}, {x1, y1, z0}, {x1, y1, z1}, {x0, y1, z1}},  // +y
            {{x0, y0, z1}, {x1, y0, z1}, {x1, y0, z0}, {x0, y0, z0}},  // -y
            {{x1, y0, z1}, {x0, y0, z1}, {x0, y1, z1}, {x1, y1, z1}},  // +z
            {{x0, y0, z0}, {x1, y0, z0}, {x1, y1, z0}, {x0, y1, z0}},  // -z
        };
        for (int f = 0; f < 6; f++) {
            scene_tri(faces[f].a[0], faces[f].a[1], faces[f].a[2], faces[f].b[0], faces[f].b[1], faces[f].b[2],
                      faces[f].c[0], faces[f].c[1], faces[f].c[2], c, lf);
            scene_tri(faces[f].a[0], faces[f].a[1], faces[f].a[2], faces[f].c[0], faces[f].c[1], faces[f].c[2],
                      faces[f].d[0], faces[f].d[1], faces[f].d[2], c, lf);
        }
    }
}

// --- The player's own readouts --------------------------------------------

#define SLOT_W   44
#define SLOT_GAP 4
#define BAR_Y    (DISPLAY_LOG_H - 56)

static void frame(pax_buf_t* fb, int x, int y, int w, int h, int t, uint32_t argb) {
    box(fb, x, y, w, t, argb);
    box(fb, x, y + h - t, w, t, argb);
    box(fb, x, y, t, h, argb);
    box(fb, x + w - t, y, t, h, argb);
}

// A heart, as a small block with its top corners knocked off. Five
// rows of rectangles rather than a sprite -- the shape reads at this
// size and it needs no texture, which is the whole reason the icons
// are colours for now.
static void heart(pax_buf_t* fb, int x, int y, uint32_t argb) {
    box(fb, x + 1, y, 3, 2, argb);
    box(fb, x + 6, y, 3, 2, argb);
    box(fb, x, y + 2, 10, 3, argb);
    box(fb, x + 1, y + 5, 8, 2, argb);
    box(fb, x + 3, y + 7, 4, 2, argb);
}

// A drumstick: a shank and a knuckle.
static void drumstick(pax_buf_t* fb, int x, int y, uint32_t argb) {
    box(fb, x, y + 5, 6, 4, argb);
    box(fb, x + 5, y + 1, 5, 6, argb);
}

// A tool's icon is a SHAPE, not just a colour.
//
// Three stone tools drawn as flat squares are three near-identical
// grey squares, and picking the wrong one is then a thing you find out
// by swinging it. A handle plus a head in the class's own outline is
// tellable apart at a glance, and it needs no texture -- which is the
// whole reason the icons are drawn rather than sampled for now. Real
// sprites (D-03) replace this without the caller changing.
#define HANDLE_ARGB 0xFF8A6432u

static void slot_icon(pax_buf_t* fb, int x, int y, uint16_t item, item_def_t d) {
    int const ix = x + 9, iy = y + 9, w = SLOT_W - 18;

    if (d.tool == TOOL_NONE) {
        box(fb, ix, iy, w, w, d.argb);  // a block, or a plain item
        return;
    }

    // The handle, corner to corner-ish, for every tool.
    box(fb, ix + w / 2 - 1, iy + 6, 3, w - 6, HANDLE_ARGB);

    switch (d.tool) {
        case TOOL_PICK:
            // A wide head with the points turned down.
            box(fb, ix + 2, iy + 4, w - 4, 4, d.argb);
            box(fb, ix + 1, iy + 2, 3, 3, d.argb);
            box(fb, ix + w - 4, iy + 2, 3, 3, d.argb);
            break;
        case TOOL_AXE:
            // A blade down one side only, which is what makes an axe an
            // axe at this size.
            box(fb, ix + w / 2, iy + 2, w / 2 - 1, 9, d.argb);
            box(fb, ix + w - 4, iy + 4, 3, 5, d.argb);
            break;
        case TOOL_SHOVEL:
            // A scoop at the bottom, where a pickaxe has nothing.
            box(fb, ix + w / 2 - 4, iy + w - 10, 9, 8, d.argb);
            break;
        default: box(fb, ix + 2, iy + 2, w - 4, 8, d.argb); break;
    }
    (void)item;
}

void hud_player(pax_buf_t* fb, player_t const* p) {
    if (fb == NULL || p == NULL) return;
    hud_begin(fb);

    int const total = INV_HOTBAR * SLOT_W + (INV_HOTBAR - 1) * SLOT_GAP;
    int const x0    = ((int)DISPLAY_LOG_W - total) / 2;

    for (int i = 0; i < INV_HOTBAR; i++) {
        int const         x = x0 + i * (SLOT_W + SLOT_GAP);
        inv_slot_t const* s = &p->inv.slot[i];

        box(fb, x, BAR_Y, SLOT_W, SLOT_W, 0xFF202028u);
        // The selected slot gets a thick light border, because at arm's
        // length on a handheld a one-pixel difference is not a
        // difference.
        bool const sel = (i == p->inv.selected);
        frame(fb, x, BAR_Y, SLOT_W, SLOT_W, sel ? 3 : 1, sel ? 0xFFFFFFFFu : 0xFF606060u);

        if (s->item == 0) continue;
        item_def_t const d = item_def(s->item);
        slot_icon(fb, x, BAR_Y, s->item, d);

        // A tool's remaining life, as a bar under the icon. It only
        // appears once the tool is actually worn, so a full inventory
        // is not a wall of green.
        if (d.durability > 0 && s->wear > 0) {
            int const w = (int)((long)(d.durability - s->wear) * (SLOT_W - 14) / d.durability);
            box(fb, x + 7, BAR_Y + SLOT_W - 8, SLOT_W - 14, 3, 0xFF202020u);
            box(fb, x + 7, BAR_Y + SLOT_W - 8, w, 3, w * 3 > SLOT_W ? 0xFF40D040u : 0xFFD04040u);
        }

        if (s->count > 1) {
            char n[8];
            snprintf(n, sizeof(n), "%d", s->count);
            // Right-aligned inside the slot, with a shadow: a white
            // number on a light block is otherwise unreadable.
            pax_vec2f const sz = rendertext_size(NULL, 16.0f, n);
            float const     tx = (float)(x + SLOT_W - 5) - sz.x, ty = (float)(BAR_Y + SLOT_W - 6) - sz.y;
            rendertext_draw(fb, 0xFF000000u, NULL, 16.0f, tx + 1.0f, ty + 1.0f, n);
            rendertext_draw(fb, 0xFFFFFFFFu, NULL, 16.0f, tx, ty, n);
        }
    }

    // Health and hunger, above the hotbar: hearts from the left, food
    // from the right, as everyone expects them.
    int const row_y = BAR_Y - 16;
    for (int i = 0; i < PL_HEALTH_MAX / 2; i++) {
        bool const full = p->health >= (i + 1) * 2;
        bool const half = !full && p->health == i * 2 + 1;
        heart(fb, x0 + i * 12, row_y, full ? 0xFFE03030u : half ? 0xFF803030u : 0xFF404040u);
    }
    for (int i = 0; i < PL_HUNGER_MAX / 2; i++) {
        bool const full = p->hunger >= (i + 1) * 2;
        drumstick(fb, x0 + total - 11 - i * 12, row_y, full ? 0xFFC08030u : 0xFF404040u);
    }
}

// The Tab screen: the same slots the hotbar draws, in the grid they
// actually live in, with the hotbar as its bottom row -- so "move this
// up to where I can reach it" is a direction rather than a rule to
// remember.
void hud_inventory(pax_buf_t* fb, player_t const* p) {
    if (fb == NULL || p == NULL || !p->inv.open) return;
    hud_begin(fb);

    int const cols = INV_HOTBAR, rows = INV_ROWS + 1;
    int const gw   = cols * SLOT_W + (cols - 1) * SLOT_GAP;
    int const gh   = rows * SLOT_W + (rows - 1) * SLOT_GAP;
    int const gx   = ((int)DISPLAY_LOG_W - gw) / 2;
    int const gy   = ((int)DISPLAY_LOG_H - gh) / 2 - 10;

    // A dim sheet over the world, so the grid reads as a panel rather
    // than as floating squares.
    // Halve the world behind the panel rather than blending a sheet
    // over it: same effect, no per-pixel alpha maths, and it keeps the
    // hue so the screen still reads as "the world, dimmed".
    direct_565_dim_rect(s_px, s_rev, 0, 0, (int)DISPLAY_LOG_W, (int)DISPLAY_LOG_H);
    box(fb, gx - 12, gy - 34, gw + 24, gh + 46, 0xFF2A2A32u);
    frame(fb, gx - 12, gy - 34, gw + 24, gh + 46, 2, 0xFF606068u);
    rendertext_draw(fb, 0xFFFFFFFFu, NULL, 22.0f, (float)(gx - 4), (float)(gy - 30), "Inventory");

    for (int i = 0; i < INV_SLOTS; i++) {
        // Row 0 of the DRAWING is the storage top; the hotbar is the
        // bottom row, which is where it is on screen when closed.
        int const col = i % INV_HOTBAR;
        int const row = inv_screen_row(i);  // the same mapping the cursor moves by

        int const x = gx + col * (SLOT_W + SLOT_GAP);
        int const y = gy + row * (SLOT_W + SLOT_GAP);

        box(fb, x, y, SLOT_W, SLOT_W, 0xFF1A1A20u);
        bool const cur = (i == p->inv.cursor);
        bool const sel = i < INV_HOTBAR && (i == p->inv.selected);
        frame(fb, x, y, SLOT_W, SLOT_W, cur ? 3 : 1, cur ? 0xFFFFD040u : sel ? 0xFFFFFFFFu : 0xFF505058u);

        inv_slot_t const* sl = &p->inv.slot[i];
        if (sl->item == 0) continue;
        slot_icon(fb, x, y, sl->item, item_def(sl->item));
        if (sl->count > 1) {
            char n[8];
            snprintf(n, sizeof(n), "%d", sl->count);
            pax_vec2f const sz = rendertext_size(NULL, 16.0f, n);
            float const     tx = (float)(x + SLOT_W - 5) - sz.x, ty = (float)(y + SLOT_W - 6) - sz.y;
            rendertext_draw(fb, 0xFF000000u, NULL, 16.0f, tx + 1.0f, ty + 1.0f, n);
            rendertext_draw(fb, 0xFFFFFFFFu, NULL, 16.0f, tx, ty, n);
        }
    }

    rendertext_draw(fb, 0xFFB0B0B8u, NULL, 16.0f, (float)(gx - 4), (float)(gy + gh + 4),
                    "cursor keys move   F1-F6 put it on the hotbar   Tab closes");
}

void hud_mine_progress(pax_buf_t* fb, float progress) {
    if (fb == NULL || progress <= 0.0f) return;
    hud_begin(fb);
    int const w = 60, h = 6;
    int const x = (int)RENDER_HALF_W - w / 2, y = (int)RENDER_HORIZON_Y + 22;
    box(fb, x, y, w, h, 0xFF202028u);
    int const fill = (int)((float)w * (progress > 1.0f ? 1.0f : progress));
    box(fb, x, y, fill, h, 0xFFE8E8E8u);
}

void hud_text_lines(pax_buf_t* fb, char const* const* lines, int n) {
    if (fb == NULL) return;
    hud_begin(fb);
    float const h = 16.0f;
    for (int i = 0; i < n; i++) {
        if (lines[i] == NULL || lines[i][0] == '\0') continue;
        float const y = 10.0f + (float)i * (h + 6.0f);
        rendertext_draw(fb, 0xFF000000u, NULL, h, 12.0f, y + 1.5f, lines[i]);
        rendertext_draw(fb, 0xFFFFFFFFu, NULL, h, 10.5f, y, lines[i]);
    }
}
