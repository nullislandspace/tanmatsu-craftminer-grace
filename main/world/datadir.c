// =====================================================================
//  SynthMiner  --  where the player's data lives (see datadir.h)
// =====================================================================

#include "world/datadir.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "world/vfs_compat.h"
#include "world/worldstore.h"

// What a build from before the data directory existed (D-80) wrote
// into its INSTALL directory. Nothing else there is the player's: the
// textures and the music beside them are what the app shipped with,
// and adopting those would put all eleven pieces into the pool twice
// (datadir.h).
// settings.tmp is settings.txt's half-written twin (ui/settings.h).
static char const* const INSTALL_ENTRIES[] = {"worlds", "settings.txt", "settings.tmp", "replays", "screenshots"};

// Everything a DATA directory holds, which is all of the above plus
// what arrived after the split: the benchmark world (step 41), the
// test kit's shots, the player's own music and their own translations.
static char const* const DATA_ENTRIES[] = {"worlds",      "bench", "settings.txt", "settings.tmp", "replays",
                                           "screenshots", "test",  "music",        "lang"};

static char const* const* entries_of(datadir_set_t set, size_t* n) {
    if (set == DD_DATA) {
        *n = sizeof(DATA_ENTRIES) / sizeof(DATA_ENTRIES[0]);
        return DATA_ENTRIES;
    }
    *n = sizeof(INSTALL_ENTRIES) / sizeof(INSTALL_ENTRIES[0]);
    return INSTALL_ENTRIES;
}

static bool exists(char const* path) {
    struct stat st;
    return stat(path, &st) == 0;
}

static void add(char* report, size_t len, char const* line) {
    if (report == NULL || len == 0) return;
    size_t const used = strlen(report);
    if (used + 1 >= len) return;
    snprintf(report + used, len - used, "%s\n", line);
}

int datadir_adopt(char const* old_base, char const* new_base, datadir_set_t set, char* report, size_t report_len) {
    if (report != NULL && report_len > 0) report[0] = '\0';
    if (!sm_mkdir_p(new_base)) return -1;
    if (old_base == NULL || strcmp(old_base, new_base) == 0) return 0;

    size_t             count = 0;
    char const* const* names = entries_of(set, &count);

    int moved = 0;
    for (size_t i = 0; i < count; i++) {
        char from[192], to[192], line[448];
        snprintf(from, sizeof(from), "%s/%s", old_base, names[i]);
        snprintf(to, sizeof(to), "%s/%s", new_base, names[i]);
        if (!exists(from)) continue;
        if (exists(to)) {
            snprintf(line, sizeof(line), "left %s: %s already exists", from, to);
        } else if (sm_rename(from, to)) {
            snprintf(line, sizeof(line), "moved %s to %s", from, to);
            moved++;
        } else {
            snprintf(line, sizeof(line), "could not move %s to %s", from, to);
        }
        add(report, report_len, line);
    }
    return moved;
}


// --- The rename ------------------------------------------------------

// One scan of `dir`, renaming at most BATCH files whose name ends in
// `from` so that it ends in `to`. Returns how many, and sets `more` if
// the batch filled up and the caller should come round again.
//
// IT DOES NOT RENAME WHILE IT IS LISTING. FatFs gives no promise about
// what a directory handle does when the directory changes under it, so
// the names are collected first and the handle closed before anything
// moves. The batch is what bounds the memory that takes.
#define BATCH 24

// The longest path this ever builds: the data directory, a world's
// slug, "region", and a region file whose coordinates are both full
// negative 32-bit numbers. That is under 128 characters; the room here
// is so that `join` never has to refuse, not because it might.
#define DD_PATH 512

// Build "a/b". False, and `out` empty, if it would not fit -- a path
// that does not fit is one that must not be renamed HALF way.
static bool join(char* out, char const* a, char const* b) {
    int const n = snprintf(out, DD_PATH, "%s/%s", a, b);
    if (n > 0 && n < DD_PATH) return true;
    out[0] = '\0';
    return false;
}

static int rename_batch(char const* dir, char const* from, char const* to, bool* more) {
    char names[BATCH][64];
    int  n   = 0;
    *more    = false;

    sm_dir_t* d = sm_dir_open(dir);
    if (d == NULL) return 0;
    char const* name;
    bool        is_dir = false;
    size_t const flen  = strlen(from);
    while ((name = sm_dir_next(d, &is_dir)) != NULL) {
        if (is_dir) continue;
        size_t const len = strlen(name);
        if (len <= flen || len >= sizeof(names[0])) continue;
        if (strcmp(name + len - flen, from) != 0) continue;
        if (n == BATCH) {
            *more = true;
            break;
        }
        snprintf(names[n++], sizeof(names[0]), "%s", name);
    }
    sm_dir_close(d);

    int done = 0;
    for (int i = 0; i < n; i++) {
        char         old_path[DD_PATH], new_path[DD_PATH], stem[sizeof(names[0])];
        size_t const len = strlen(names[i]);
        snprintf(stem, sizeof(stem), "%.*s%s", (int)(len - flen), names[i], to);
        if (!join(old_path, dir, names[i]) || !join(new_path, dir, stem)) continue;
        if (sm_rename(old_path, new_path)) done++;
    }
    return done;
}

static int rename_ext(char const* dir, char const* from, char const* to) {
    int  total = 0;
    bool more  = true;
    while (more) {
        int const n = rename_batch(dir, from, to, &more);
        total += n;
        if (n == 0) break;  // nothing moved: another round would not either
    }
    return total;
}

// One world directory: its level file and its regions.
static int rename_world(char const* dir) {
    int  n = 0;
    char from[DD_PATH], to[DD_PATH], region[DD_PATH];
    if (join(from, dir, "level.cmw") && join(to, dir, "level.smw")) {
        if (exists(from) && !exists(to) && sm_rename(from, to)) n++;
    }
    if (join(region, dir, "region")) n += rename_ext(region, ".cmr", ".smr");
    return n;
}

int datadir_rename_saves(char const* base, char* report, size_t report_len) {
    if (report != NULL && report_len > 0) report[0] = '\0';
    if (base == NULL || *base == '\0') return -1;

    int  total = 0;
    char worlds[DD_PATH];
    if (!join(worlds, base, "worlds")) return -1;

    // The player's worlds. Their names are collected before any of them
    // is touched, for the same reason rename_batch does it.
    char slugs[SM_WORLDS_MAX][SM_WORLD_SLUG_MAX];
    int  ns = 0;
    sm_dir_t* d = sm_dir_open(worlds);
    if (d != NULL) {
        char const* name;
        // EVERY entry, not only the ones that say they are directories.
        // worldstore_list() trusts that flag and is right to -- on the
        // badge it comes from FatFs's AM_DIR -- but this runs once at
        // start with nothing to gain from being clever, and an entry
        // that is really a file (worlds.idx) simply has no level.cmw
        // and no region/ inside it, so it costs two failed opens.
        while ((name = sm_dir_next(d, NULL)) != NULL && ns < SM_WORLDS_MAX) {
            if (name[0] == '.' || strlen(name) >= sizeof(slugs[0])) continue;
            snprintf(slugs[ns++], sizeof(slugs[0]), "%s", name);
        }
        sm_dir_close(d);
    }
    for (int i = 0; i < ns; i++) {
        char dir[DD_PATH], line[DD_PATH + 64];
        if (!join(dir, worlds, slugs[i])) continue;
        int const n = rename_world(dir);
        if (n > 0) {
            snprintf(line, sizeof(line), "renamed %d file(s) in %s", n, dir);
            add(report, report_len, line);
        }
        total += n;
    }

    // The benchmark world, which lives beside worlds/ rather than in
    // it (worldstore.h), and the replays.
    char other[DD_PATH], line[DD_PATH + 64];
    if (!join(other, base, "bench")) return total;
    int n = rename_world(other);
    if (n > 0) {
        snprintf(line, sizeof(line), "renamed %d file(s) in %s", n, other);
        add(report, report_len, line);
    }
    total += n;

    if (!join(other, base, "replays")) return total;
    n = rename_ext(other, ".cmr", ".smr");
    if (n > 0) {
        snprintf(line, sizeof(line), "renamed %d replay(s) in %s", n, other);
        add(report, report_len, line);
    }
    total += n;

    return total;
}

// --- Getting rid of the old one ---------------------------------------

// How deep the old directories go: an install directory is
// <slug>/textures/<file> and a world is worlds/<slug>/region/<file>,
// so three. Eight is far past either, and stops a directory that
// somehow points at itself from taking the stack with it.
#define RETIRE_DEPTH 8

// Everything inside `path`, then nothing else -- `path` itself is the
// caller's to remove, because the caller is the one that checked it.
//
// A directory is one that can be LISTED, rather than one the listing
// said was a directory. The flag is right on the badge (it comes from
// FatFs's AM_DIR) and this would work either way, but a wrong answer
// here deletes the wrong thing, and that is not a thing to be nearly
// sure about.
static int wipe(char const* path, int depth) {
    if (depth > RETIRE_DEPTH) return 0;

    int removed = 0;
    for (;;) {
        // Collected before anything goes, and in batches, for the same
        // reason rename_batch does it: nothing changes a directory
        // while it is holding that directory open.
        char names[BATCH][64];
        int  n = 0;

        sm_dir_t* d = sm_dir_open(path);
        if (d == NULL) break;  // not a directory, or already gone
        char const* name;
        while (n < BATCH && (name = sm_dir_next(d, NULL)) != NULL) {
            if (strlen(name) >= sizeof(names[0])) continue;
            snprintf(names[n++], sizeof(names[0]), "%s", name);
        }
        sm_dir_close(d);
        if (n == 0) break;

        int did = 0;
        for (int i = 0; i < n; i++) {
            char child[DD_PATH];
            if (!join(child, path, names[i])) continue;
            did += wipe(child, depth + 1);  // empties it, if it is one
            if (sm_remove(child)) did++;    // then takes it, file or directory
        }
        // Nothing budged, so another round would find the same names
        // and fail at them again. Stop instead of spinning.
        if (did == 0) break;
        removed += did;
    }
    return removed;
}

// Is `parent` an ancestor of `path`, or the same place?
static bool covers(char const* parent, char const* path) {
    size_t const n = strlen(parent);
    if (strncmp(parent, path, n) != 0) return false;
    return path[n] == '\0' || path[n] == '/';
}

int datadir_retire(char const* dir, char const* keep, datadir_set_t set, char* report, size_t report_len) {
    if (report != NULL && report_len > 0) report[0] = '\0';
    if (dir == NULL || keep == NULL) return -1;

    char line[448];

    // The path has to be a real one, and not one that reaches the live
    // directory. `covers` catches both "it IS the live one" and "it is
    // a parent of it", which is what would turn a wrong constant into
    // deleting /sd/apps.
    if (dir[0] == '\0' || strcmp(dir, "/") == 0 || strstr(dir, "..") != NULL) {
        snprintf(line, sizeof(line), "kept %s: not a path to delete", dir);
        add(report, report_len, line);
        return -1;
    }
    if (covers(dir, keep)) {
        snprintf(line, sizeof(line), "kept %s: %s is inside it", dir, keep);
        add(report, report_len, line);
        return -1;
    }
    if (!exists(dir)) return 0;  // already gone, which is the usual case

    // THE GUARD THAT MATTERS. Adoption leaves an entry whose
    // destination already exists, on purpose, and that entry is
    // somebody's world. If anything is still in here, this directory
    // is not finished with and must not be deleted.
    size_t             count = 0;
    char const* const* names = entries_of(set, &count);
    for (size_t i = 0; i < count; i++) {
        char path[DD_PATH];
        if (!join(path, dir, names[i])) continue;
        if (!exists(path)) continue;
        snprintf(line, sizeof(line), "kept %s: %s is still in it", dir, names[i]);
        add(report, report_len, line);
        return -1;
    }

    int n = wipe(dir, 0);
    if (sm_remove(dir)) n++;
    if (exists(dir)) {
        snprintf(line, sizeof(line), "could not remove %s (%d entries went)", dir, n);
        add(report, report_len, line);
        return n;
    }
    snprintf(line, sizeof(line), "removed %s (%d entries)", dir, n);
    add(report, report_len, line);
    return n;
}
