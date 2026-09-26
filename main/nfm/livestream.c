// =====================================================================
//  livestream  --  the switch behind the Display menu (livestream.h)
// =====================================================================

#include "livestream.h"

#include "esp_log.h"
#include "stream.h"
#include "usbnet.h"

// What the encoder is asked for. The game's own rate varies with what
// is on screen, so `fps_hint` is not a promise -- it is what the rate
// control and the PTS clock are scaled against, and a keyframe every
// `gop` frames is about one a second at the rate this game actually
// runs (step 43).
#define LS_FPS_HINT 20
#define LS_GOP      20
#define LS_BITRATE  3000  // kbit/s, the nfmtest default

static char const TAG[] = "livestream";

static bool s_on;

bool livestream_on(void) {
    return s_on;
}

void livestream_frame(pax_buf_t* fb) {
    if (s_on) stream_publish(fb);
}

bool livestream_set(bool on, pax_buf_t const* fb) {
    if (on == s_on) return s_on;

    if (!on) {
        // Down in the order it came up. The console comes back with
        // usbnet_stop(), so nothing logged before that line arrives.
        stream_stop();
        usbnet_stop();
        s_on = false;
        ESP_LOGI(TAG, "livestream off: the console is back");
        return false;
    }

    if (fb == NULL) return false;

    // EVERYTHING THAT CAN FAIL AND BE REPORTED HAPPENS FIRST. After
    // usbnet_start() there is no console to report anything on, so the
    // encoder and its buffers are checked while there is still one --
    // which means preparing before the link, and tearing down again if
    // the link is the part that fails.
    stream_cfg_t const cfg = {.br_kbit = LS_BITRATE, .gop = LS_GOP, .fps_hint = LS_FPS_HINT};
    esp_err_t const    err = stream_prepare(&cfg, fb);
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "livestream: the encoder would not start (%s)", esp_err_to_name(err));
        return false;
    }

    ESP_LOGW(TAG, "livestream on: the console and BadgeLink go away now, until it is switched off");
    if (usbnet_start(NULL, false) != ESP_OK) {
        // The console is still here: usbnet_start undoes its own half.
        stream_stop();
        ESP_LOGE(TAG, "livestream: the USB network would not come up");
        return false;
    }
    stream_start();
    s_on = true;
    return true;
}

void livestream_shutdown(void) {
    if (s_on) livestream_set(false, NULL);
}
