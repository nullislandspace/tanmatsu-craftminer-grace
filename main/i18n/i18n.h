#pragma once
// =====================================================================
//  CraftMiner  --  the UI in the player's language
// ---------------------------------------------------------------------
//  Every word the game shows comes from here. `lang/*.txt` holds them,
//  one file per language, plain `key = text` lines in UTF-8;
//  `tools/make_lang.py` bakes those into the arrays in strings_gen.c,
//  which is what ships. So a translator edits a text file, and the game
//  pays one array index -- no parsing, no lookup by name, nothing per
//  frame (D-81).
//
//  English is the reference: `lang/en.txt` defines the keys, and a
//  language missing one falls back to the English text at GENERATION
//  time, so every array is full and nothing has to be checked at run
//  time.
//
//  A player who cannot read the current language is not stuck: the
//  language names in the menu each stand in their own language, and the
//  setting is two rows into Settings.
//
//  WHAT IS NOT TRANSLATED: the name CraftMiner; world names, which the
//  player types; the key names (Esc, Left Shift) printed on the badge's
//  own keys; and every log line, which is for us, not for a player.
// =====================================================================

#include <stdarg.h>
#include <stdbool.h>
#include <stddef.h>

#include "i18n/strings_gen.h"  // cm_str_t, CM_STR_COUNT, cm_lang_t, CM_LANG_COUNT

// The text for a string, in the current language. Never NULL, and the
// pointer stays good until the language changes -- menus that hold
// labels for one frame are fine; anything kept longer should copy.
char const* i18n_text(cm_str_t s);

// What the call sites read. Short on purpose: it appears a few hundred
// times, and a longer name would push rows of menu code off the line.
#define T(s) i18n_text(s)

cm_lang_t   i18n_language(void);
void        i18n_set_language(cm_lang_t lang);

// The language's own name for itself ("Deutsch"), and the code that
// goes in settings.txt ("de", "nl-BE").
char const* i18n_language_name(cm_lang_t lang);
char const* i18n_language_code(cm_lang_t lang);

// The language a code names. False (and *out untouched) for a code from
// a newer build or a typo, which leaves the caller on its default.
bool i18n_language_from_code(char const* code, cm_lang_t* out);

// --- Filling in the blanks --------------------------------------------------

// snprintf for a translated format string, and the reason this module
// has code in it at all.
//
// A translation may need the values in a different order than English
// puts them: "3 blocks from the edge" against "vom Rand 3 Blöcke". The
// printf way to say that is `%2$s`, which is a POSIX extension the
// badge's libc is not promised to have -- so the substitution is done
// here instead, with the C library only ever asked to format ONE value
// at a time, which every libc can do.
//
//   i18n_fmt(buf, sizeof buf, CM_STR_WORLD_SUB, slot + 1, seed);
//
// The TYPES come from the English string, never from the translation:
// `%d` in en.txt is read as an int whatever the translation writes
// there, so a hand-edited lang file on the SD card can get the padding
// wrong, or drop a value, but can never make this read the wrong kind
// of argument off the stack. A `%N$` beyond what English declares is
// dropped. `make check` catches all of that long before a player does.
//
// Returns the length it wanted to write, like snprintf.
int i18n_fmt(char* buf, size_t cap, cm_str_t s, ...);
int i18n_vfmt(char* buf, size_t cap, cm_str_t s, va_list ap);

// The most values one string may take. Raising it costs nothing but a
// little stack; `make check` fails on a string that needs more.
#define I18N_FMT_MAX_ARGS 6

// --- Translations a player can fix themselves -------------------------------

// Read `<dir>/lang/<code>.txt` (CM_DATA_DIR, datadir.h) over the baked
// text, for the current language, and keep it until the language
// changes. Same format as lang/*.txt; an unknown key is ignored and a
// missing one keeps what was baked in, so a file with one line in it
// changes one string.
//
// This is what makes the shipped translations -- which start out as a
// machine's work -- fixable by whoever actually speaks the language,
// with a text editor and no toolchain at all. Nothing here trusts the
// file: see i18n_fmt above.
void i18n_load_overrides(char const* dir);

// --- For the host checks only -----------------------------------------------

#ifdef CM_HOST
// Put `text` in place of a string, or NULL to take it back: what an
// override file would do, without a file. worldcheck uses it to feed
// i18n_fmt the sort of format string a stranger's lang file might hold.
void i18n_test_override(cm_str_t s, char const* text);
#endif
