#pragma once
// =====================================================================
//  CraftMiner  --  "how many?"
// ---------------------------------------------------------------------
//  A small modal over whatever screen asked for it: moving a stack of
//  more than one into or out of a chest, or into a furnace, asks first
//  (the user's call, 2026-09-23). Taking a furnace's OUTPUT does not --
//  there is never a reason to leave half a smelt behind.
//
//  It starts at everything, because that is what the key used to do and
//  the common case has not changed. A slider AND a number, since one
//  answers "roughly half" and the other answers "exactly seventeen" and
//  neither answers both.
//
//  The keys are the arrows and the digits, and nothing else: it is a
//  modal, so it takes the keyboard whole while it is up.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "bsp/input.h"
#include "pax_gfx.h"

// Ask for a number between 1 and `max`, about `item`. Starts at `max`.
void amount_open(uint16_t item, int max);
bool amount_active(void);
void amount_event(bsp_input_event_t const* ev);

// Once a frame. Returns AMOUNT_PENDING while it is still up,
// AMOUNT_CANCELLED if it was dismissed, or the chosen number.
#define AMOUNT_PENDING   (-1)
#define AMOUNT_CANCELLED (0)
int  amount_update(void);
void amount_draw(pax_buf_t* fb);
