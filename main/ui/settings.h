#pragma once
// =====================================================================
//  CraftMiner  --  the game's own settings
// ---------------------------------------------------------------------
//  What the Graphics, Audio and Controls menus change -- key bindings
//  included -- in ONE TEXT FILE ON THE SD CARD, next to the worlds:
//
//      /sd/craftminer/settings.txt
//
//  so copying that directory backs up a player's worlds and their
//  settings together (D-67). Not the app's install directory: the
//  launcher owns that, and may empty it on an update (datadir.h, D-80). It is plain `key=value` lines: readable, and
//  fixable by hand if it ever needs to be. Unknown keys are ignored and
//  missing ones keep their defaults, so the file survives settings being
//  added and removed, the way level.cmw survives new tags.
//
//  NOT here: volume and the three brightnesses. Those are the device's,
//  shared with the launcher and every other app, and se_hw.h owns them;
//  the menus call it directly. A game keeping its own copy of the volume
//  is how a badge ends up loud in one app and silent in the next.
//
//  Each change rewrites the file at once. A setting is changed by a
//  person pressing a key in a menu, a few times a session at most, so
//  there is nothing to batch. The rewrite goes to settings.tmp first and
//  is renamed over the old file, so a badge switched off mid-write keeps
//  one whole copy or the other.
// =====================================================================

#include <stdbool.h>

// View distance: an index into cm_view_preset() -- 0 near, 1 medium,
// 2 far.
#define SETTINGS_VIEW_COUNT 3
// What a player gets before choosing: near, the user's call (D-76) --
// medium was, until its cost on foot was measured (F-66).
#define SETTINGS_VIEW_DEFAULT 0

// The language lives here too, as its code ("de"), but it is i18n.h
// that holds it: settings_load hands it to i18n_set_language, and
// settings_save asks i18n_language() what to write. There is no
// settings_language() -- one copy of that state, not two.

// Read settings.txt from `dir` (CM_DATA_DIR, datadir.h). Missing
// keys keep their defaults (English, near, textured, half resolution,
// music and effects on, gyroscope off, every key its default binding).
// Call once at boot, AFTER input_init(): the key bindings it restores are
// the ones input_init registered.
void settings_load(char const* dir);

// Write the file now. The setters below do it themselves; the key
// bindings call it after a rebind (input.c).
void settings_save(void);

int  settings_view(void);
void settings_set_view(int view);

bool settings_textured(void);
void settings_set_textured(bool on);

// Half resolution: the scene is drawn at a quarter of the pixels and
// scaled up. The fast default (D-06); full resolution is sharper and
// about half the frame rate.
bool settings_half_res(void);
void settings_set_half_res(bool on);

// The drifting clouds: a couple of hundred flat triangles and a lot of
// sky to fill, so they can be switched off. On by default.
bool settings_clouds(void);
void settings_set_clouds(bool on);

// Third person: the camera behind Fred instead of behind his eyes.
// First person, with his arm and what it holds, by default.
bool settings_third_person(void);
void settings_set_third_person(bool on);

// Which hand Fred holds things in, in both views. Right by default.
bool settings_left_handed(void);
void settings_set_left_handed(bool on);

// The game has no sound yet (block 14). These are stored now so the
// menu exists and a player's choice is already remembered when it
// does; setting them also switches the engine mixer's gates, so they
// take effect the moment there is anything to hear.
bool settings_music(void);
void settings_set_music(bool on);
bool settings_sfx(void);
void settings_set_sfx(bool on);

// Looking round by turning the badge (input.h, input_gyro_frame). Off by
// default; the cursor keys work either way.
bool settings_gyro(void);
void settings_set_gyro(bool on);

// The mixer group the game's sound effects play on (se_audio.h assigns
// group numbers no meaning; this is ours).
#define SETTINGS_SFX_GROUP 0
