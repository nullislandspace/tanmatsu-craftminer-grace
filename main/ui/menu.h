#pragma once
// =====================================================================
//  SynthMiner  --  the menus
// ---------------------------------------------------------------------
//  Every screen that is not the game itself:
//
//    title bar      Play / Settings / Quit, under the word in blocks
//      Play         the save slots
//        a world    Play / Rename / Delete
//        empty      New world: name, seed, Create
//      Settings     Controls / Graphics / Audio / Display
//    pause          Resume / Save / Settings / Save and quit to title
//
//  Drawn with the engine's list menu (se_ui.h) over whatever is behind
//  it -- the title's drift, or the frozen world -- so nothing needs a
//  screen of its own.
//
//  THE MENU DECIDES, main.c ACTS. Anything that changes which world is
//  open or whether the game runs comes back from menu_update() as a
//  command, because the world switch has an order to it (drain, clear,
//  open; see main.c) that belongs in one place. Things that are the
//  menu's own business -- a setting, a key binding, renaming or
//  deleting a world nobody has open -- it simply does.
//
//  INPUT IS EVENTS, not polling. A key press arrives as up to three
//  events (scancode, navigation, character), and which ones depends on
//  the keyboard; menu_event() folds them into one action per key per
//  frame, so a press on either keyboard moves the cursor exactly once.
// =====================================================================

#include <stdbool.h>
#include <stdint.h>

#include "bsp/input.h"
#include "pax_gfx.h"
#include "world/worldstore.h"

typedef enum {
    MENU_CMD_NONE = 0,
    MENU_CMD_PLAY,       // open the world in `slot`
    MENU_CMD_CREATE,     // make a world in `slot` from `name` and `seed`, then play it
    MENU_CMD_RESUME,     // close the pause menu
    MENU_CMD_SAVE,       // save the open world, stay paused
    MENU_CMD_SAVE_QUIT,  // save the open world and go back to the title
    MENU_CMD_LEAVE,      // back to the launcher
    MENU_CMD_GRAPHICS,   // a graphics setting changed: apply it
} menu_cmd_kind_t;

typedef struct {
    menu_cmd_kind_t kind;
    int             slot;
    char            name[SM_WORLD_NAME_MAX];
    uint32_t        seed;
} menu_cmd_t;

// The title's bar (its root) or the pause menu. Either replaces
// whatever menu was showing.
void menu_open_title(void);
void menu_open_pause(void);

// No menu at all: the game has the keyboard.
void menu_close(void);
bool menu_active(void);

// Is the menu showing something other than the title's bar? The title
// hides its "press a key" line under a panel.
bool menu_in_panel(void);

// Every input event the engine hands the app while a menu is active.
void menu_event(bsp_input_event_t const* ev);

// Once a frame: act on this frame's keys.
menu_cmd_t menu_update(void);

// Draw the menu over `fb`, at full resolution, after everything else.
void menu_draw(pax_buf_t* fb);

// Open one screen by name ("worlds", "controls", "pause", ...) over the
// title, for a `shots` test: the menus are drawn from state, so a shot
// of each is a regression test of each. False for a name it does not
// know.
bool menu_show(char const* name);

// A line of news under the menu title for a few seconds ("Saved").
void menu_status(char const* msg);
