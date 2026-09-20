// =====================================================================
//  Showreel  --  where the horizon lies on screen (see horizon.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/horizon.c. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/horizon.h"
#include <math.h>
#include "synthengine3d.h"

// How far away the two probe points are placed. 20 km against eye
// heights of a few units puts the error far below a pixel.
#define HORIZON_PROBE_DIST 20000.0f

// Steepest slope used for a vertical horizon: crosses 480 rows within a
// hundredth of a pixel.
#define HORIZON_MAX_SLOPE 1.0e5f

horizon_t horizon_current(void) {
    render_camera_t const cam = render_camera();

    // Forward and right on the level plane, from the camera's yaw. The
    // probes sit 45 degrees to either side of the view direction, level
    // with the eye: in front of the camera for any pitch short of
    // straight up or down, whatever the roll.
    float const fx = sinf(cam.yaw), fz = cosf(cam.yaw);
    float const rx = cosf(cam.yaw), rz = -sinf(cam.yaw);
    float const d = HORIZON_PROBE_DIST;

    float lsx, lsy, rsx, rsy;
    render_project(cam.x + fx * d - rx * d, cam.y, cam.z + fz * d - rz * d, &lsx, &lsy);
    render_project(cam.x + fx * d + rx * d, cam.y, cam.z + fz * d + rz * d, &rsx, &rsy);

    float dx = rsx - lsx;
    if (fabsf(dx) < 1.0e-3f) dx = dx < 0.0f ? -1.0e-3f : 1.0e-3f;
    float slope = (rsy - lsy) / dx;
    if (slope > HORIZON_MAX_SLOPE) slope = HORIZON_MAX_SLOPE;
    if (slope < -HORIZON_MAX_SLOPE) slope = -HORIZON_MAX_SLOPE;

    // World up, seen by the camera, is its up axis's y component:
    // cos(pitch) * cos(roll) (the engine's basis, M = Ry * Rx * Rz). While
    // that is positive the sky is towards the top of the screen.
    bool const upright = cosf(cam.pitch) * cosf(cam.roll) >= 0.0f;

    return (horizon_t){
        .y0           = lsy - slope * lsx,  // extrapolated back to x = 0
        .slope        = slope,
        .ground_below = upright,
    };
}
