// =====================================================================
//  stream  --  the game's frame -> PPA -> H.264 -> MPEG-TS -> UDP
// ---------------------------------------------------------------------
//  Ported from tanmatsu-nfmtest-grace (main/nfm/stream.c). The encoder,
//  PPA and muxer calls are that file's, unchanged; the frame source and
//  the division of work between tasks are not (see stream.h).
// =====================================================================

#include "stream.h"

#include <string.h>

#include "driver/ppa.h"
#include "esp_cache.h"
#include "esp_h264_enc_single.h"
#include "esp_h264_enc_single_hw.h"
#include "esp_h264_types.h"
#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "tsmux.h"
#include "usbnet.h"

#define OUT_W     800
#define OUT_H     480
#define YUV_BYTES (OUT_W * OUT_H * 3 / 2)
#define PORT      5000

#define STREAM_PRIO 5  // below usbnet (10), on its core
#define STREAM_CORE 1
#define TASK_STACK  6144
#define SEND_WAIT   2  // ticks a datagram may wait for room in the ring

// TWO YUV PLANES, not three framebuffers. The game's own framebuffer is
// never handed over -- it is read by the PPA while the caller still owns
// it -- so what crosses between tasks is the converted frame, which the
// game never touches. One is being filled, one is being encoded.
#define NYUV 2

static stream_cfg_t          s_cfg;
static stream_stats_t        s_st;
static uint8_t*              s_yuv[NYUV];
static uint8_t*              s_bs;
static ppa_client_handle_t   s_ppa;
static esp_h264_enc_handle_t s_enc;
static tsmux_t               s_mux;
static volatile bool         s_run;
static volatile bool         s_done;
static volatile int          s_ready;  // filled and waiting for the encoder, -1 none
static int                   s_fill;   // the one stream_publish writes into
static uint32_t              s_seq;    // frames encoded, for the PTS

static bool emit(void* ctx, uint8_t const* d, size_t len) {
    (void)ctx;
    return usbnet_send_udp(PORT, d, (uint16_t)len, SEND_WAIT);
}

bool stream_running(void) {
    return s_run;
}

// --- the frame path: runs in the caller, so keep it short ----------------

void stream_publish(pax_buf_t const* fb) {
    if (!s_run || fb == NULL) return;
    s_st.published++;

    // The encoder still has the last one. Drop this frame rather than
    // block the game waiting for it.
    if (s_ready >= 0) {
        s_st.dropped++;
        return;
    }

    void* const pixels = pax_buf_get_pixels_rw((pax_buf_t*)fb);
    if (pixels == NULL) {
        s_st.ppa_errors++;
        return;
    }
    // The game drew with the CPU, so what the PPA is about to read has
    // to be out of the cache first.
    esp_cache_msync(pixels, (size_t)OUT_W * OUT_H * 2, ESP_CACHE_MSYNC_FLAG_DIR_C2M);

    int64_t const               t0  = esp_timer_get_time();
    ppa_srm_oper_config_t const srm = {
        .in =
            {
                .buffer  = pixels,
                .pic_w   = (uint32_t)pax_buf_get_width_raw(fb),
                .pic_h   = (uint32_t)pax_buf_get_height_raw(fb),
                .block_w = (uint32_t)pax_buf_get_width_raw(fb),
                .block_h = (uint32_t)pax_buf_get_height_raw(fb),
                .srm_cm  = PPA_SRM_COLOR_MODE_RGB565,
            },
        .out =
            {
                .buffer      = s_yuv[s_fill],
                .buffer_size = YUV_BYTES,
                .pic_w       = OUT_W,
                .pic_h       = OUT_H,
                .srm_cm      = PPA_SRM_COLOR_MODE_YUV420,
                .yuv_range   = PPA_COLOR_RANGE_LIMIT,
                .yuv_std     = PPA_COLOR_CONV_STD_RGB_YUV_BT601,
            },
        .rotation_angle = PPA_SRM_ROTATION_ANGLE_90,  // undoes the panel's rotation
        .scale_x        = 1.0f,
        .scale_y        = 1.0f,
        .mode           = PPA_TRANS_MODE_BLOCKING,
    };
    if (ppa_do_scale_rotate_mirror(s_ppa, &srm) != ESP_OK) {
        s_st.ppa_errors++;
        return;
    }
    uint32_t const us = (uint32_t)(esp_timer_get_time() - t0);
    if (us > s_st.ppa_us_max) s_st.ppa_us_max = us;

    // Published last, and only now: until this line the stream task has
    // no claim on the buffer.
    int const filled = s_fill;
    s_fill           = (s_fill + 1) % NYUV;
    __sync_synchronize();
    s_ready = filled;
}

// --- the stream task: everything expensive ------------------------------

static void stream_task(void* arg) {
    (void)arg;
    while (s_run) {
        int const b = s_ready;
        if (b < 0) {
            vTaskDelay(1);
            continue;
        }

        // The PTS is derived from the frames actually sent and the rate
        // the encoder was configured for. The game's rate varies; the
        // player only needs a clock that advances evenly.
        uint64_t const pts = 90000 + (uint64_t)s_seq * 90000 / (uint64_t)s_cfg.fps_hint;

        esp_h264_enc_in_frame_t  in   = {.raw_data = {.buffer = s_yuv[b], .len = YUV_BYTES}, .pts = (uint32_t)pts};
        esp_h264_enc_out_frame_t out  = {.raw_data = {.buffer = s_bs, .len = YUV_BYTES}};
        int64_t const            t1   = esp_timer_get_time();
        esp_h264_err_t const     herr = esp_h264_enc_process(s_enc, &in, &out);
        int64_t const            t2   = esp_timer_get_time();

        // Released before the muxing, so the game may fill it again
        // while this frame is still going out over USB.
        __sync_synchronize();
        s_ready = -1;
        s_seq++;

        if (herr != ESP_H264_ERR_OK || out.length == 0) {
            s_st.enc_errors++;
            continue;
        }
        bool const key = out.frame_type == ESP_H264_FRAME_TYPE_IDR || out.frame_type == ESP_H264_FRAME_TYPE_I;
        tsmux_write(&s_mux, s_bs, out.length, pts, key);
        int64_t const t3 = esp_timer_get_time();

        s_st.frames++;
        if (key) s_st.keyframes++;
        s_st.es_bytes += out.length;
        s_st.dgrams        = s_mux.dgrams;
        s_st.dgrams_failed = s_mux.dgrams_failed;
        s_st.ts_bytes      = s_mux.bytes;
        uint32_t const enc = (uint32_t)(t2 - t1), mux = (uint32_t)(t3 - t2);
        if (enc > s_st.enc_us_max) s_st.enc_us_max = enc;
        if (mux > s_st.mux_us_max) s_st.mux_us_max = mux;
    }
    s_done = true;
    vTaskDelete(NULL);
}

// --- set up and tear down -----------------------------------------------

static void free_all(void) {
    if (s_enc) {
        esp_h264_enc_close(s_enc);
        esp_h264_enc_del(s_enc);
        s_enc = NULL;
    }
    if (s_ppa) {
        ppa_unregister_client(s_ppa);
        s_ppa = NULL;
    }
    for (int i = 0; i < NYUV; i++) {
        heap_caps_free(s_yuv[i]);
        s_yuv[i] = NULL;
    }
    heap_caps_free(s_bs);
    s_bs = NULL;
}

esp_err_t stream_prepare(stream_cfg_t const* cfg, pax_buf_t const* tmpl) {
    s_cfg = *cfg;
    if (s_cfg.fps_hint < 1) s_cfg.fps_hint = 30;
    if (s_cfg.gop < 1) s_cfg.gop = s_cfg.fps_hint;
    memset(&s_st, 0, sizeof(s_st));

    int const w = pax_buf_get_width_raw(tmpl), h = pax_buf_get_height_raw(tmpl);
    if (pax_buf_get_type(tmpl) != PAX_BUF_16_565RGB || w * h != OUT_W * OUT_H) return ESP_ERR_NOT_SUPPORTED;

    for (int i = 0; i < NYUV; i++) {
        s_yuv[i] = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);
        if (!s_yuv[i]) goto nomem;
    }
    s_bs = heap_caps_aligned_calloc(64, 1, YUV_BYTES, MALLOC_CAP_SPIRAM);  // >= input, or the encoder refuses
    if (!s_bs) goto nomem;

    ppa_client_config_t const pcfg = {
        .oper_type             = PPA_OPERATION_SRM,
        .max_pending_trans_num = 1,
        .data_burst_length     = PPA_DATA_BURST_LENGTH_128,
    };
    if (ppa_register_client(&pcfg, &s_ppa) != ESP_OK) {
        free_all();
        return ESP_FAIL;
    }
    esp_h264_enc_cfg_hw_t const ecfg = {
        .pic_type = ESP_H264_RAW_FMT_O_UYY_E_VYY,
        .gop      = (uint8_t)s_cfg.gop,
        .fps      = (uint8_t)s_cfg.fps_hint,
        .res      = {.width = OUT_W, .height = OUT_H},
        .rc       = {.bitrate = s_cfg.br_kbit * 1000u, .qp_min = 16, .qp_max = 40},
    };
    if (esp_h264_enc_hw_new(&ecfg, &s_enc) != ESP_H264_ERR_OK || s_enc == NULL ||
        esp_h264_enc_open(s_enc) != ESP_H264_ERR_OK) {
        free_all();
        return ESP_FAIL;
    }
    tsmux_init(&s_mux, emit, NULL);
    return ESP_OK;

nomem:
    free_all();
    return ESP_ERR_NO_MEM;
}

void stream_start(void) {
    s_ready = -1;
    s_fill  = 0;
    s_seq   = 0;
    s_done  = false;
    s_run   = true;
    xTaskCreatePinnedToCore(stream_task, "sm_stream", TASK_STACK, NULL, STREAM_PRIO, NULL, STREAM_CORE);
}

void stream_stop(void) {
    s_run = false;
    for (int i = 0; !s_done && i < 200; i++) vTaskDelay(pdMS_TO_TICKS(10));
    free_all();
}

void stream_get_stats(stream_stats_t* out) {
    *out = s_st;
}
