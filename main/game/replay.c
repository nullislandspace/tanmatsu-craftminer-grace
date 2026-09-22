// =====================================================================
//  CraftMiner  --  recording and replaying play (see replay.h)
// =====================================================================

#include "game/replay.h"

#include <stdio.h>
#include <string.h>

#include "common/psram.h"
#include "items/items.h"

#define REPLAY_MAGIC   "CMRP"
// 2: the inventory by item NAME, not number. Item numbers follow the
// last block id (items.h), so they move every time a block is added;
// names never do (D-74). A version-1 file is refused.
#define REPLAY_VERSION 2u

typedef struct {
    uint32_t mask;
    float    yaw, pitch;
} rtick_t;

static rtick_t*       s_ticks;  // REPLAY_MAX_TICKS of them, in PSRAM, allocated on first use
static int            s_n, s_pos;
static bool           s_rec, s_play;
static replay_start_t s_start;

static bool buffer(void) {
    if (s_ticks == NULL) s_ticks = cm_alloc((size_t)REPLAY_MAX_TICKS * sizeof(rtick_t));
    return s_ticks != NULL;
}

// --- Recording ------------------------------------------------------------

bool replay_record_begin(replay_start_t const* start) {
    if (start == NULL || !buffer()) return false;
    s_start = *start;
    s_n     = 0;
    s_rec   = true;
    s_play  = false;
    return true;
}

bool replay_recording(void) {
    return s_rec;
}

void replay_record_tick(uint32_t mask, float gyro_yaw, float gyro_pitch) {
    if (!s_rec || s_n >= REPLAY_MAX_TICKS) return;
    s_ticks[s_n++] = (rtick_t){mask, gyro_yaw, gyro_pitch};
}

// Field by field rather than struct by struct, so the file does not
// depend on how a compiler pads a struct -- the host checks and the badge
// must agree about it.
static bool put(FILE* f, void const* p, size_t n) {
    return fwrite(p, 1, n, f) == n;
}
static bool get(FILE* f, void* p, size_t n) {
    return fread(p, 1, n, f) == n;
}

static bool write_start(FILE* f, replay_start_t const* s) {
    bool ok = put(f, &s->seed, 4) && put(f, &s->time_of_day, 8) && put(f, &s->x, 8) && put(f, &s->y, 8) &&
              put(f, &s->z, 8) && put(f, &s->yaw, 4) && put(f, &s->pitch, 4) && put(f, &s->selected, 4);
    for (int i = 0; ok && i < INV_SLOTS; i++) {
        char const* const name  = s->inv[i].item != 0 ? item_def(s->inv[i].item).name : "";
        uint8_t const     len   = (uint8_t)strlen(name);
        uint8_t const     count = s->inv[i].count;
        ok = put(f, &len, 1) && put(f, name, len) && put(f, &count, 1) && put(f, &s->inv[i].wear, 2);
    }
    return ok;
}

static bool read_start(FILE* f, replay_start_t* s) {
    memset(s, 0, sizeof(*s));
    bool ok = get(f, &s->seed, 4) && get(f, &s->time_of_day, 8) && get(f, &s->x, 8) && get(f, &s->y, 8) &&
              get(f, &s->z, 8) && get(f, &s->yaw, 4) && get(f, &s->pitch, 4) && get(f, &s->selected, 4);
    for (int i = 0; ok && i < INV_SLOTS; i++) {
        uint8_t len = 0, count = 0;
        char    name[64];
        ok = get(f, &len, 1) && len < sizeof(name) && get(f, name, len) && get(f, &count, 1) &&
             get(f, &s->inv[i].wear, 2);
        if (!ok) break;
        name[len]        = '\0';
        s->inv[i].item  = item_by_name(name);  // 0 for "" or a name this build lacks
        s->inv[i].count = s->inv[i].item != 0 ? count : 0;
    }
    return ok;
}

bool replay_record_end(char const* path) {
    if (!s_rec) return false;
    s_rec = false;
    if (path == NULL) return true;
    FILE* f = fopen(path, "wb");
    if (f == NULL) return false;
    uint32_t const version = REPLAY_VERSION, n = (uint32_t)s_n;
    bool           ok = put(f, REPLAY_MAGIC, 4) && put(f, &version, 4) && put(f, &n, 4) && write_start(f, &s_start);
    for (int i = 0; ok && i < s_n; i++) {
        ok = put(f, &s_ticks[i].mask, 4) && put(f, &s_ticks[i].yaw, 4) && put(f, &s_ticks[i].pitch, 4);
    }
    ok = ok && fflush(f) == 0;
    fclose(f);
    return ok;
}

// --- Playing ----------------------------------------------------------------

bool replay_load(char const* path, replay_start_t* start) {
    s_play = false;
    if (path == NULL || start == NULL || !buffer()) return false;
    FILE* f = fopen(path, "rb");
    if (f == NULL) return false;
    char     magic[4];
    uint32_t version = 0, n = 0;
    bool     ok = get(f, magic, 4) && memcmp(magic, REPLAY_MAGIC, 4) == 0 && get(f, &version, 4) &&
              version == REPLAY_VERSION && get(f, &n, 4) && n <= REPLAY_MAX_TICKS && read_start(f, &s_start);
    for (uint32_t i = 0; ok && i < n; i++) {
        ok = get(f, &s_ticks[i].mask, 4) && get(f, &s_ticks[i].yaw, 4) && get(f, &s_ticks[i].pitch, 4);
    }
    fclose(f);
    if (!ok) return false;
    s_n    = (int)n;
    s_pos  = 0;
    s_play = true;
    s_rec  = false;
    *start = s_start;
    return true;
}

bool replay_playing(void) {
    return s_play;
}

bool replay_next(uint32_t* mask, float* gyro_yaw, float* gyro_pitch) {
    if (!s_play || s_pos >= s_n) {
        s_play = false;
        return false;
    }
    rtick_t const t = s_ticks[s_pos++];
    if (mask != NULL) *mask = t.mask;
    if (gyro_yaw != NULL) *gyro_yaw = t.yaw;
    if (gyro_pitch != NULL) *gyro_pitch = t.pitch;
    return true;
}

int replay_position(void) {
    return s_pos;
}

int replay_length(void) {
    return s_n;
}

void replay_stop(void) {
    s_play = false;
}
