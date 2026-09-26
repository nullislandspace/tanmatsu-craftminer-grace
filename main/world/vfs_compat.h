#pragma once
// =====================================================================
//  SynthMiner  --  the file operations graceloader does not export
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
//  On the host (SM_HOST) all of this is plain stdio, so the region and
//  world-store checks run on a PC.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>

// Delete a FILE. True if it is gone afterwards (including if it was
// never there).
bool sm_remove(char const* path);

// Delete an EMPTY DIRECTORY. True if it is gone afterwards.
//
// A SEPARATE CALL because f_unlink will not do it (F-94): it answers
// FR_DENIED for a directory, and sm_remove then works its way down the
// candidate spellings until one of them says FR_NO_FILE -- which it
// used to read as "already gone" and report success for, leaving the
// directory exactly where it was and saying it had not. POSIX `rmdir`
// IS exported by graceloader, like `mkdir` and `stat` and unlike
// `remove`, so this needs none of that machinery.
bool sm_rmdir(char const* path);

// Rename / move a file. True on success.
bool sm_rename(char const* from, char const* to);

// Create a directory and every missing parent. True if it exists
// afterwards.
bool sm_mkdir_p(char const* path);

// --- Listing ----------------------------------------------------------
//
// Enough to enumerate the world directory, and no more.

typedef struct sm_dir sm_dir_t;

// Open a directory for listing, or NULL.
sm_dir_t* sm_dir_open(char const* path);

// The next entry's name, or NULL at the end. `is_dir` may be NULL.
// The name points at storage owned by the handle and is valid until the
// next call.
char const* sm_dir_next(sm_dir_t* d, bool* is_dir);

void sm_dir_close(sm_dir_t* d);
