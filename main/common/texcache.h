#pragma once
// =====================================================================
//  Showreel asset  --  shared texture cache
// ---------------------------------------------------------------------
//  Assets ask for textures by file name; each file is loaded once (from
//  the app's install directory, into PSRAM) and shared, so the station
//  and the marauders can both use plate_gunmetal.png without two copies.
//  Everything is unloaded together by texcache_shutdown().
//  Lifted from tanmatsu-showreel-grace,
//  main/common/texcache.h. Changes here are CraftMiner's;
//  the showreel stays the origin to diff against.
// =====================================================================

#include "synthengine3d.h"

// Where the texture files are. Call before the first texcache_get().
void texcache_init(char const* asset_dir);
void texcache_shutdown(void);

// The texture loaded from `file`, or NULL if it failed to load (logged
// once; the caller draws flat instead).
se_texture_t const* texcache_get(char const* file);
