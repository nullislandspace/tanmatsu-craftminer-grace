#pragma once
// =====================================================================
//  CraftMiner  --  the file operations graceloader does not export
// ---------------------------------------------------------------------
//  stdio is exported and works: fopen, fread, fwrite, fseek, ftell,
//  fflush, fclose, mkdir, stat. Four things are NOT (checked against
//  fakelib/liball.so, F-06):
//
//      remove   rename   unlink   opendir/readdir
//
//  An app that calls them links fine and then fails to LOAD, because
//  the symbol cannot be resolved at load time -- the worst kind of
//  failure, since nothing goes wrong until the badge runs it. So
//  deleting, renaming and listing go through FatFs (f_unlink, f_rename,
//  f_opendir) instead, which IS exported.
//
//  FatFs paths are volume-relative and carry no VFS mount point, so the
//  "/sd/apps/..." a caller has is not a FatFs path. Rather than hard-code
//  a mapping that would break silently if the mount changed, the
//  plausible spellings are tried and whichever works is kept -- the same
//  approach, for the same reason, as synthengine3D/src/se_mp3.c:198.
//
//  On the host (CM_HOST) all of this is plain stdio, so the region and
//  world-store checks run on a PC.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>

// Delete a file. True if it is gone afterwards (including if it was
// never there).
bool cm_remove(char const* path);

// Rename / move a file. True on success.
bool cm_rename(char const* from, char const* to);

// Create a directory and every missing parent. True if it exists
// afterwards.
bool cm_mkdir_p(char const* path);

// --- Listing ----------------------------------------------------------
//
// Enough to enumerate the world directory, and no more.

typedef struct cm_dir cm_dir_t;

// Open a directory for listing, or NULL.
cm_dir_t* cm_dir_open(char const* path);

// The next entry's name, or NULL at the end. `is_dir` may be NULL.
// The name points at storage owned by the handle and is valid until the
// next call.
char const* cm_dir_next(cm_dir_t* d, bool* is_dir);

void cm_dir_close(cm_dir_t* d);
