// =====================================================================
//  Showreel  --  the backdrop (see backdrop.h)
//  Lifted from tanmatsu-showreel-grace,
//  main/backdrop.c. Changes here are SynthMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "voxel/backdrop.h"
#include <math.h>
#include "esp_log.h"
#include "synthengine3d.h"
#include "voxel/horizon.h"

static char const TAG[] = "backdrop";

// PPA job ids: the frame drains its own jobs, so they restart every frame.
#define JOB_TOP    0u
#define JOB_BOTTOM 1u

#define OPAQUE 0xFF000000u

static bool s_ppa;

// This frame, from backdrop_begin for backdrop_finish, so the two halves
// cannot disagree about where the line is.
static bool      s_split;  // sky and ground (else one colour)
static horizon_t s_hz;
static int       s_band;        // first row of the bottom region
static uint32_t  s_col_top;     // rows [0, s_band) and the side above the line
static uint32_t  s_col_bottom;  // rows [s_band, h) and the side below the line
static bool      s_top_queued, s_bottom_queued;
static float     s_scale;  // screen pixels per buffer pixel: 1, or 2 at quarter resolution

// The horizon's row in buffer pixels at buffer column x.
static float line_y(float x) {
    return horizon_y(&s_hz, x * s_scale) / s_scale;
}

// direct_565_vrun() for either buffer: the same rotated layout, the
// buffer's own stride (its raw width).
static void vrun(pax_buf_t* fb, int x, int y0, int y1, uint16_t packed) {
    int const w = (int)pax_buf_get_width(fb), h = (int)pax_buf_get_height(fb);
    int const raw_w = (int)pax_buf_get_width_raw(fb);
    if (x < 0 || x >= w) return;
    if (y0 < 0) y0 = 0;
    if (y1 > h - 1) y1 = h - 1;
    uint16_t* p = (uint16_t*)pax_buf_get_pixels(fb) + x * raw_w + (raw_w - 1 - y0);
    for (int n = y1 - y0 + 1; n > 0; n--) *p-- = packed;
}

void backdrop_init(void) {
    s_ppa = se_ppa_init();
    if (!s_ppa) ESP_LOGW(TAG, "PPA unavailable -- the backdrop falls back to the CPU");
}

void backdrop_begin(pax_buf_t* fb, backdrop_t const* bd) {
    int const w = (int)pax_buf_get_width(fb);
    int const h = (int)pax_buf_get_height(fb);

    s_scale = (float)DISPLAY_LOG_W / (float)w;
    s_split = bd != NULL && bd->ground;
    if (!s_split) {
        s_col_top = (bd ? bd->sky_argb : 0u) | OPAQUE;
        s_band    = h;
    } else {
        s_hz               = horizon_current();
        uint32_t const sky = bd->sky_argb | OPAQUE;
        uint32_t const gnd = bd->ground_argb | OPAQUE;
        s_col_top          = s_hz.ground_below ? sky : gnd;
        s_col_bottom       = s_hz.ground_below ? gnd : sky;
        // The lowest point the line reaches across the screen: every row
        // below it is wholly on the bottom side, every row above it is on
        // the top side except for the wedge the tilt cuts out, which the
        // CPU paints afterwards (with the bottom colour).
        float const lowest = fmaxf(line_y(0.0f), line_y((float)(w - 1)));
        float const band   = ceilf(fminf(fmaxf(lowest, -1.0f), (float)h)) + 1.0f;
        s_band             = band < 0.0f ? 0 : band > (float)h ? h : (int)band;
    }

    // The hardware paints both regions whole; the CPU only fills in.
    s_top_queued    = s_ppa && s_band > 0 && se_ppa_fill(fb, JOB_TOP, 0, s_band, s_col_top);
    s_bottom_queued = s_ppa && s_split && s_band < h && se_ppa_fill(fb, JOB_BOTTOM, s_band, h - s_band, s_col_bottom);
}

void backdrop_finish(pax_buf_t* fb) {
    // Wait FIRST, before touching a single framebuffer pixel. The CPU's
    // rows and the PPA's are disjoint, but not their cache lines: a line
    // straddling the seam, held dirty by the CPU and written back after
    // the DMA, clobbers the fill (Stunt Racer, Race the Synth).
    if (s_top_queued) se_ppa_wait_job(JOB_TOP);
    if (s_bottom_queued) se_ppa_wait_job(JOB_BOTTOM);

    int const      w      = (int)pax_buf_get_width(fb);
    int const      h      = (int)pax_buf_get_height(fb);
    uint16_t const top    = direct_565_pack_for(fb, s_col_top);
    uint16_t const bottom = direct_565_pack_for(fb, s_col_bottom);

    // Whatever the PPA refused (queue full) or could not do (no PPA).
    if (!s_top_queued && s_band > 0) {
        if (!s_split) {
            pax_background(fb, s_col_top);
        } else {
            for (int x = 0; x < w; x++) vrun(fb, x, 0, s_band - 1, top);
        }
    }
    if (s_split && !s_bottom_queued && s_band < h) {
        for (int x = 0; x < w; x++) vrun(fb, x, s_band, h - 1, bottom);
    }
    if (!s_split) return;

    // The wedge: in each column, from the line down to the band, the
    // bottom side's colour. Nothing to do in a column whose line is at
    // the band already -- every column when the camera is level.
    for (int x = 0; x < w; x++) {
        float const y  = ceilf(line_y((float)x));
        int const   hy = y < 0.0f ? 0 : y > (float)s_band ? s_band : (int)y;
        if (hy < s_band) vrun(fb, x, hy, s_band - 1, bottom);
    }
}
