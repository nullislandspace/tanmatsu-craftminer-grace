#pragma once
// =====================================================================
//  stream  --  the game's frame -> PPA -> H.264 -> MPEG-TS -> UDP :5000
// ---------------------------------------------------------------------
//  Ported from tanmatsu-nfmtest-grace (main/nfm/stream.{c,h}), where it
//  was Part B of that project's plan. What is different here is WHERE
//  THE FRAMES COME FROM, and it changes the shape of the thing:
//
//    nfmtest   drew its own test pattern in its own task and a stream
//              task took the newest one AT A FIXED RATE, skipping slots
//              it could not keep up with.
//    here      the game draws, and every finished frame is offered
//              (stream_publish, from on_render). There is no rate and
//              no clock: the stream runs at whatever the game runs at.
//
//  SO THE SPLIT OF WORK MOVED TOO. The colour conversion happens INLINE,
//  in the caller, because that is the only moment the game's framebuffer
//  is known to be still: the engine flips pages, and a frame handed to
//  another task would be drawn over while it was read. The PPA does it
//  in a couple of milliseconds. Everything expensive after that -- the
//  encoder, the muxer, the USB -- is done by the stream task, off the
//  frame path.
//
//  If the encoder has not finished with the previous frame, the new one
//  is DROPPED rather than waited for (stream_stats_t.dropped). A stream
//  that stutters is better than a game that does.
//
//  THE CONSOLE GOES AWAY WHILE THIS RUNS. usbnet takes the USB-C PHY off
//  the serial console and gives it to the OTG controller (usbnet.h, its
//  F-07), so for as long as the stream is up there is no console, no
//  BadgeLink and no log output. That is why the setting behind it is not
//  saved and always starts off (D-95): the only way back is the menu
//  that turned it on, or a reset.
//
//  On the PC: OBS -> Sources -> Media Source, untick Local File, input
//  `udp://@:5000`, format `mpegts`. Or: ffplay -fflags nobuffer
//  udp://@:5000
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "esp_err.h"
#include "pax_gfx.h"

typedef struct {
    uint32_t br_kbit;  // encoder target
    int      gop;      // frames between keyframes
    int      fps_hint; // what the encoder is TOLD, for its rate control
} stream_cfg_t;

typedef struct {
    uint32_t published;      // frames offered by the game
    uint32_t dropped;        // offered while the encoder was still busy
    uint32_t frames;         // encoded and muxed
    uint32_t keyframes;
    uint32_t enc_errors;
    uint32_t ppa_errors;
    uint64_t es_bytes;       // H.264 bytes out of the encoder
    uint32_t dgrams;
    uint32_t dgrams_failed;  // usbnet would not take them (no link, ring full)
    uint64_t ts_bytes;
    uint32_t ppa_us_max, enc_us_max, mux_us_max;
} stream_stats_t;

// Buffers, PPA client and encoder. `tmpl` is the framebuffer the game
// draws into: its size, format and orientation are read from it. Call
// before the link is up, so a failure still reaches the console.
esp_err_t stream_prepare(stream_cfg_t const* cfg, pax_buf_t const* tmpl);

void stream_start(void);
void stream_stop(void);  // stops the task, frees everything

// True between start and stop. `stream_publish` is a no-op otherwise, so
// the frame path does not have to ask.
bool stream_running(void);

// Offer the finished frame. Converts it to YUV420 (the PPA, ~2 ms) and
// hands it to the stream task; drops it if the previous one is still
// being encoded. Safe to call when the stream is not running.
//
// CALL IT WITH THE FRAME STILL YOURS -- from inside on_render, before
// the engine presents and moves on to the next page.
void stream_publish(pax_buf_t const* fb);

void stream_get_stats(stream_stats_t* out);
