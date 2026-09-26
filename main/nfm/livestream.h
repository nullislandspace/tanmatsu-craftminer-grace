#pragma once
// =====================================================================
//  livestream  --  the switch behind the Display menu's last row
// ---------------------------------------------------------------------
//  One flag, and the two subsystems it turns on in the right order:
//
//      usbnet   the USB-C port becomes a network adapter (usbnet.h)
//      stream   each finished frame -> PPA -> H.264 -> MPEG-TS -> UDP
//
//  usbnet first, because the encoder's buffers are worth allocating only
//  if the link came up, and stream_prepare() logs its failures -- which
//  it can still do at that point, but not after.
//
//  IT IS NOT SAVED, AND IT ALWAYS STARTS OFF (D-95). Turning it on takes
//  the USB-C PHY away from the serial console and gives it to the OTG
//  controller (usbnet.h, F-07), so while it runs there is no console, no
//  BadgeLink, and no log output at all. A setting that could persist
//  would be a setting that could lock the badge out of its own
//  development link across a restart, with nothing on screen to say why.
//  Off at every start means the way out is always a power cycle at
//  worst, and the same menu row at best.
//
//  The player's own settings.txt is untouched: this never reaches it
//  (ui/settings.h holds what IS saved).
// =====================================================================

#include <stdbool.h>

#include "pax_gfx.h"

// Is it on? Cheap; the frame path asks every frame.
bool livestream_on(void);

// Turn it on or off. `fb` is the framebuffer the game draws into,
// needed to size the encoder; it may be NULL when switching off.
// Returns what the state IS afterwards, which is not always what was
// asked for -- the link may not come up, or the encoder may not fit --
// so the menu row can show the truth rather than the intention.
bool livestream_set(bool on, pax_buf_t const* fb);

// Offer the finished frame, from inside on_render while it is still
// ours. A no-op when off.
void livestream_frame(pax_buf_t* fb);

// Off, if it is on: for leaving the game. Safe to call always.
void livestream_shutdown(void);
