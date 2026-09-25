#pragma once
// =====================================================================
//  SynthMiner  --  folding text down to what the badge can type
// ---------------------------------------------------------------------
//  THE TANMATSU HAS ONE FIXED QWERTY, and the game speaks 32 languages.
//  A player reading "Кирка" cannot type К; neither can a Turk type the
//  ö in "Kömür", a Pole the ł in "Łopata" or a Czech the ř in "Dřevo".
//  So the crafting book's search box can never match what is on the
//  screen. It matches a FOLDED form of it, and folds what was typed the
//  same way:
//
//      Кирка -> kirka      Kömür -> komur      Dřevo -> drevo
//
//  Accented Latin loses its accent, Cyrillic and Greek transliterate,
//  everything is lowercased, and anything that is not a letter or a
//  digit is dropped -- so spaces and hyphens never get in the way of a
//  substring match ("iron pick" folds to "ironpick", which is inside
//  "ironpickaxe").
//
//  ONE LETTER, NOT A DIGRAPH: o-umlaut folds to "o", not "oe" (the
//  user's call, 2026-09-23). A German would type "loeffel" and a Turk
//  "komur"; only one of those can win, and the single base letter is
//  right for more of the 32 languages than the digraph is.
//
//  WHERE THE TABLE CAME FROM. The Latin half is mechanical -- Unicode
//  NFD, keep the base letter -- derived over every character that
//  occurs in any lang/*.txt, plus the letters no decomposition reaches
//  (eszett, ash, slashed o, stroked d and l, thorn, eth, dotless i).
//  The Cyrillic and Greek halves are editorial and hand-written,
//  because shcha is "shch" and chi is "ch" and no algorithm knows that.
//  To regenerate after a language is added: the derivation is in
//  tools/make_fold.py, and it refuses to emit a table with a hole in it.
//
//  AND THE TABLE IS CHECKED AGAINST THE WHOLE DOMAIN, not against a
//  sample: worldcheck walks every string of every language and asserts
//  each character folds. A translation using a letter this table does
//  not know fails `make check` on the machine that builds it, rather
//  than shipping a recipe nobody can find.
//
//  Pure: no engine, no allocation. tools/worldcheck.c builds it as-is.
// =====================================================================

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

// The longest useful search key. A folded item name is far shorter;
// this is the buffer the search box and the callers size themselves to.
#define FOLD_MAX 64

// Fold `in` into `out`, always NUL-terminated, never more than cap-1
// characters. The result contains nothing but [a-z0-9].
void fold_text(char const* in, char* out, size_t cap);

// Is `folded_needle` -- already put through fold_text -- inside the
// folded form of `text`? An empty needle matches everything, which is
// what an empty search box should do.
bool fold_match(char const* text, char const* folded_needle);

// --- The pieces the checks need ---------------------------------------

// Step one codepoint of UTF-8. Returns the next position, and stores
// the codepoint (U+FFFD for a malformed byte, so a bad lang file walks
// forward rather than looping). Returns NULL at the end of the string.
char const* fold_utf8_next(char const* s, uint32_t* cp);

// Does the table know how to fold `cp`? True for every ASCII
// codepoint. This is what proves the table covers the domain.
bool fold_known(uint32_t cp);
