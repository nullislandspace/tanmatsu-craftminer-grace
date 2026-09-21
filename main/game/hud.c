// =====================================================================
//  CraftMiner  --  what is drawn over the world (see hud.h)
// =====================================================================

#include "game/hud.h"

#include "shapes/pax_misc.h"
#include "synthengine3d.h"
#include "world/chunk.h"
#include "world/chunk_render.h"

// Black, and drawn a hair outside the block's own faces so the two do
// not fight over the same depth. The engine already nudges an edge
// towards the camera (se_scene.c, SCENE_LINE_BIAS); this is the
// belt to that pair of braces, and it is what makes the outline read
// as a box round the block rather than as dashes on it.
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
    if (x < 0 || y < 0 || x >= DISPLAY_LOG_W || y >= DISPLAY_LOG_H) return;
    pax_col_t const c = pax_get_pixel(fb, x, y);
    pax_set_pixel(fb, (c & 0xFF000000u) | (~c & 0x00FFFFFFu), x, y);
}

void hud_crosshair(pax_buf_t* fb) {
    if (fb == NULL) return;
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
