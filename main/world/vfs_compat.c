// =====================================================================
//  CraftMiner  --  the file operations graceloader does not export
//                  (see vfs_compat.h)
// =====================================================================

#include "world/vfs_compat.h"

#include <stdio.h>
#include <string.h>

#include <sys/stat.h>

// mkdir is exported on the badge and exists on the host; only its
// header differs.
#ifndef CM_HOST
#include <sys/types.h>
#endif

bool cm_mkdir_p(char const* path) {
    if (path == NULL || *path == '\0') return false;

    char buf[192];
    size_t const n = strlen(path);
    if (n >= sizeof(buf)) return false;
    memcpy(buf, path, n + 1);

    // Walk the separators, creating as we go. An existing directory is
    // success, not failure -- which is the whole point of the _p.
    for (char* p = buf + 1; *p != '\0'; p++) {
        if (*p != '/') continue;
        *p = '\0';
        mkdir(buf, 0777);
        *p = '/';
    }
    mkdir(buf, 0777);

    struct stat st;
    return stat(buf, &st) == 0;
}

#ifdef CM_HOST

// ---- Host: plain stdio and dirent -----------------------------------

#include <dirent.h>
#include <stdlib.h>

bool cm_remove(char const* path) {
    if (remove(path) == 0) return true;
    // Already gone counts as success, to match the badge's f_unlink
    // returning FR_NO_FILE.
    struct stat st;
    return stat(path, &st) != 0;
}

bool cm_rename(char const* from, char const* to) {
    return rename(from, to) == 0;
}

struct cm_dir {
    DIR* d;
};

cm_dir_t* cm_dir_open(char const* path) {
    DIR* d = opendir(path);
    if (d == NULL) return NULL;
    cm_dir_t* h = malloc(sizeof(*h));
    if (h == NULL) {
        closedir(d);
        return NULL;
    }
    h->d = d;
    return h;
}

char const* cm_dir_next(cm_dir_t* h, bool* is_dir) {
    if (h == NULL) return NULL;
    struct dirent* e;
    while ((e = readdir(h->d)) != NULL) {
        if (strcmp(e->d_name, ".") == 0 || strcmp(e->d_name, "..") == 0) continue;
        if (is_dir != NULL) *is_dir = (e->d_type == DT_DIR);
        return e->d_name;
    }
    return NULL;
}

void cm_dir_close(cm_dir_t* h) {
    if (h == NULL) return;
    closedir(h->d);
    free(h);
}

#else

// ---- Badge: FatFs, because the POSIX wrappers are not exported ------

#include "esp_heap_caps.h"
#include "ff.h"

// "/sd/apps/x" is a VFS path; FatFs wants it volume-relative. Try the
// plausible spellings and keep whichever the filesystem accepts
// (se_mp3.c:198 does the same, for the same reason).
static void fat_candidate(char* out, size_t cap, char const* vfs, int which) {
    char const* rel = vfs;
    if (strncmp(vfs, "/sd", 3) == 0) {
        rel = vfs + 3;
    } else if (strncmp(vfs, "/int", 4) == 0) {
        rel = vfs + 4;
    }
    if (*rel == '\0') rel = "/";

    switch (which) {
        case 0:  snprintf(out, cap, "%s", rel);     break;
        case 1:  snprintf(out, cap, "0:%s", rel);   break;
        case 2:  snprintf(out, cap, "1:%s", rel);   break;
        default: snprintf(out, cap, "%s", vfs);     break;
    }
}

#define FAT_TRIES 4

bool cm_remove(char const* path) {
    char cand[160];
    for (int i = 0; i < FAT_TRIES; i++) {
        fat_candidate(cand, sizeof(cand), path, i);
        FRESULT const r = f_unlink(cand);
        if (r == FR_OK || r == FR_NO_FILE) return true;
    }
    return false;
}

bool cm_rename(char const* from, char const* to) {
    char a[160], b[160];
    for (int i = 0; i < FAT_TRIES; i++) {
        fat_candidate(a, sizeof(a), from, i);
        fat_candidate(b, sizeof(b), to, i);
        // f_rename's destination is relative to the source's volume, so
        // strip a drive prefix from it if the spelling added one.
        char const* dst = b;
        if (b[0] != '\0' && b[1] == ':') dst = b + 2;
        if (f_rename(a, dst) == FR_OK) return true;
    }
    return false;
}

struct cm_dir {
    FF_DIR  d;
    FILINFO info;
};

cm_dir_t* cm_dir_open(char const* path) {
    cm_dir_t* h = heap_caps_malloc(sizeof(*h), MALLOC_CAP_SPIRAM);
    if (h == NULL) return NULL;
    char cand[160];
    for (int i = 0; i < FAT_TRIES; i++) {
        fat_candidate(cand, sizeof(cand), path, i);
        if (f_opendir(&h->d, cand) == FR_OK) return h;
    }
    heap_caps_free(h);
    return NULL;
}

char const* cm_dir_next(cm_dir_t* h, bool* is_dir) {
    if (h == NULL) return NULL;
    while (f_readdir(&h->d, &h->info) == FR_OK && h->info.fname[0] != '\0') {
        if (is_dir != NULL) *is_dir = (h->info.fattrib & AM_DIR) != 0;
        return h->info.fname;
    }
    return NULL;
}

void cm_dir_close(cm_dir_t* h) {
    if (h == NULL) return;
    f_closedir(&h->d);
    heap_caps_free(h);
}

#endif
