#pragma once
// =====================================================================
//  SynthMiner  --  key-cap icons
// ---------------------------------------------------------------------
//  Ported from tanmatsu-synthracer-grace, main/icons.h. Changes here are
//  SynthMiner's; synthracer stays the origin to diff against.
// =====================================================================

#include <stdbool.h>

#include "pax_gfx.h"

// Function-key icons loaded from /int/icons/*.png on the Tanmatsu at
// boot. The launcher installs a fresh set there. We decode each PNG
// into an ARGB pax_buf_t kept in PSRAM for the lifetime of the app and
// composite them into the framebuffer with pax_draw_image
// (which honors the source alpha channel).
//
// Missing or undecodable files are tolerated: icons_get() returns NULL
// and the HUD should fall back to a plain text label.

typedef enum {
    ICON_ESC = 0,
    ICON_F1,
    ICON_F2,
    ICON_F3,
    ICON_F4,
    ICON_F5,
    ICON_F6,
    ICON_KEY_COUNT,
} icon_key_t;

// Decode all icons from /int/icons/. Safe to call once at boot, after
// the /int filesystem is mounted (graceloader does this for us before
// app_main is entered).
void icons_load(void);

// Returns NULL if the icon failed to load.
pax_buf_t* icons_get(icon_key_t key);

bool icons_any_missing(void);
int  icons_width(icon_key_t key);   // 0 if not loaded
int  icons_height(icon_key_t key);  // 0 if not loaded

// Composite the icon onto `fb` at the given user coordinates. No-op if
// the icon is missing.
void icons_blit(pax_buf_t* fb, icon_key_t key, float dst_x, float dst_y);
