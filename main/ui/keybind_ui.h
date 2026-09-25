#pragma once
// =====================================================================
//  SynthMiner  --  keybind value rendering
// ---------------------------------------------------------------------
//  Renders a bound key (scancode) as a key-cap icon (Esc / F1..F6 PNGs
//  from icons.c) or a text label. The only public entry is the se_ui
//  SE_MENU_VAL_CUSTOM value drawer for the Controls menu's keybind rows.
//  Ported from tanmatsu-synthracer-grace, main/keybind_ui.h. Changes
//  here are SynthMiner's; synthracer stays the origin to diff against.
// =====================================================================

#include "pax_gfx.h"  // pax_buf_t, pax_col_t

// se_ui SE_MENU_VAL_CUSTOM value drawer for the Controls keybind rows:
// renders the bound key as an icon/label. `ctx` carries the scancode
// packed as the pointer value.
void controls_keybind_draw(pax_buf_t* fb, float x, float y, float h, pax_col_t col, void* ctx);
