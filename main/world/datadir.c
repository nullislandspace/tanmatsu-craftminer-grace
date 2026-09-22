// =====================================================================
//  CraftMiner  --  where the player's data lives (see datadir.h)
// =====================================================================

#include "world/datadir.h"

#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "world/vfs_compat.h"

// What a build before /sd/craftminer wrote into the install directory.
// settings.tmp is settings.txt's half-written twin (ui/settings.h).
static char const* const ENTRIES[] = {"worlds", "settings.txt", "settings.tmp", "replays", "screenshots"};

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

int datadir_adopt(char const* old_base, char const* new_base, char* report, size_t report_len) {
    if (report != NULL && report_len > 0) report[0] = '\0';
    if (!cm_mkdir_p(new_base)) return -1;
    if (old_base == NULL || strcmp(old_base, new_base) == 0) return 0;

    int moved = 0;
    for (size_t i = 0; i < sizeof(ENTRIES) / sizeof(ENTRIES[0]); i++) {
        char from[192], to[192], line[448];
        snprintf(from, sizeof(from), "%s/%s", old_base, ENTRIES[i]);
        snprintf(to, sizeof(to), "%s/%s", new_base, ENTRIES[i]);
        if (!exists(from)) continue;
        if (exists(to)) {
            snprintf(line, sizeof(line), "left %s: %s already exists", from, to);
        } else if (cm_rename(from, to)) {
            snprintf(line, sizeof(line), "moved %s to %s", from, to);
            moved++;
        } else {
            snprintf(line, sizeof(line), "could not move %s to %s", from, to);
        }
        add(report, report_len, line);
    }
    return moved;
}
