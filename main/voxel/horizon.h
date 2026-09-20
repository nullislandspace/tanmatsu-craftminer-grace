#pragma once
// =====================================================================
//  Showreel  --  where the horizon lies on screen
// ---------------------------------------------------------------------
//  Every point of a level plane at infinity projects onto one straight
//  screen line, the plane's vanishing line: the horizon. Two far points
//  at eye height, to either side of the view, define it (from Stunt
//  Racer's backdrop). Unlike Stunt Racer's, the camera here may roll
//  past 90 degrees (barrel rolls, a steep take-off), so this also says
//  which side of the line the ground is on.
//
//  Uses only the engine's camera (render_camera / render_project), so
//  the host-side check (make scenecheck) tests it against its stand-in.
//  Lifted from tanmatsu-showreel-grace,
//  main/horizon.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include <stdbool.h>

typedef struct {
    // Screen line y = y0 + slope * x (logical pixels).
    float y0, slope;
    // The ground is on the side of larger y ("below" the line) -- false
    // when the camera is upside down.
    bool  ground_below;
} horizon_t;

// The horizon for the current camera. Never fails: a (near) vertical
// horizon, the camera rolled by 90 degrees, comes back as a very steep
// line, which is still right column by column.
horizon_t horizon_current(void);

// The line's y in screen column x.
static inline float horizon_y(horizon_t const* h, float x) {
    return h->y0 + h->slope * x;
}
