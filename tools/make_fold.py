#!/usr/bin/env python3
"""CraftMiner -- generate main/i18n/fold_table.h from lang/*.txt.

The badge has one fixed QWERTY and the game speaks 32 languages, so the
crafting book's search box matches a FOLDED form of every name: accents
stripped, Cyrillic and Greek transliterated, lowercased, everything that
is not a letter or a digit dropped. See main/i18n/fold.h.

THE DOMAIN IS EVERY CHARACTER IN EVERY LANG FILE, not a sample of them.
This script collects that set, folds each character, and REFUSES TO EMIT
A TABLE WITH A HOLE IN IT -- a language whose alphabet brings a letter
no rule below reaches fails here, with the character named, rather than
shipping a recipe nobody can search for.

Two halves, and they are different kinds of thing:

  * Latin is MECHANICAL. Unicode NFD, keep the base letter. Plus the
    letters no decomposition reaches, listed in LATIN below.
  * Cyrillic and Greek are EDITORIAL and written out by hand, because
    shcha is "shch", chi is "ch", and no algorithm knows that.

One letter, not a digraph: o-umlaut folds to "o", not "oe" (the user's
call, 2026-09-23).

    python3 tools/make_fold.py            # write the table
    python3 tools/make_fold.py --check    # is the committed one current?
"""

import glob
import os
import sys
import unicodedata

HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(HERE, "main", "i18n", "fold_table.h")

# Latin letters nothing mechanical reaches: these have no accent to
# strip, they ARE their own letter.
LATIN = {
    "ß": "ss", "æ": "ae", "œ": "oe", "ø": "o", "đ": "d", "ł": "l",
    "þ": "th", "ð": "d", "ı": "i", "İ": "i", "ŋ": "ng",
}

# Greek, in the shape a person typing on a Latin keyboard would reach
# for: beta as v and eta as i, which is how modern Greek sounds and how
# Greeks themselves write it in Latin letters.
GREEK = {
    "α": "a", "β": "v", "γ": "g", "δ": "d", "ε": "e", "ζ": "z",
    "η": "i", "θ": "th", "ι": "i", "κ": "k", "λ": "l", "μ": "m",
    "ν": "n", "ξ": "x", "ο": "o", "π": "p", "ρ": "r", "σ": "s",
    "ς": "s", "τ": "t", "υ": "y", "φ": "f", "χ": "ch", "ψ": "ps",
    "ω": "o",
    "ά": "a", "έ": "e", "ή": "i", "ί": "i", "ό": "o", "ύ": "y",
    "ώ": "o", "ϊ": "i", "ϋ": "y", "ΐ": "i", "ΰ": "y",
}

# Cyrillic. The hard sign folds to "a" because in Bulgarian it is a
# vowel and is written that way in Latin (Bulgaria); the soft sign folds
# to nothing, because in Russian and Ukrainian it is silent.
CYRILLIC = {
    "а": "a", "б": "b", "в": "v", "г": "g", "д": "d", "е": "e",
    "ё": "e", "ж": "zh", "з": "z", "и": "i", "й": "y", "к": "k",
    "л": "l", "м": "m", "н": "n", "о": "o", "п": "p", "р": "r",
    "с": "s", "т": "t", "у": "u", "ф": "f", "х": "h", "ц": "ts",
    "ч": "ch", "ш": "sh", "щ": "shch", "ъ": "a", "ы": "y", "ь": "",
    "э": "e", "ю": "yu", "я": "ya",
    "ђ": "dj", "є": "ye", "і": "i", "ї": "yi", "ј": "j", "љ": "lj",
    "њ": "nj", "ћ": "c", "џ": "dz", "ґ": "g",
}


def fold_char(ch):
    """The ASCII a character folds to, or None if no rule reaches it."""
    if ch in LATIN:
        return LATIN[ch]
    lo = ch.lower()
    for table in (LATIN, GREEK, CYRILLIC):
        if lo in table:
            return table[lo]
    base = "".join(c for c in unicodedata.normalize("NFD", lo)
                   if not unicodedata.combining(c))
    if base.isascii() and base.isalnum():
        return base.lower()
    # Quotation marks, dashes, the middle dot, the inverted question
    # mark: punctuation, and a search box is better off without it.
    if unicodedata.category(ch)[0] in "PZS":
        return ""
    return None


def domain():
    """Every non-ASCII character in every lang file, with who uses it."""
    seen = {}
    files = sorted(glob.glob(os.path.join(HERE, "lang", "*.txt")))
    if not files:
        sys.exit("make_fold: no lang/*.txt found")
    for path in files:
        code = os.path.basename(path)[:-4]
        with open(path, encoding="utf-8") as f:
            for line in f:
                line = line.strip()
                if not line or line.startswith("#") or "=" not in line:
                    continue
                for ch in line.split("=", 1)[1]:
                    if ord(ch) > 127:
                        seen.setdefault(ch, set()).add(code)
    return seen


def build():
    seen = domain()
    table, holes = {}, []
    for ch, langs in seen.items():
        rep = fold_char(ch)
        if rep is None:
            holes.append((ch, langs))
        else:
            table[ord(ch)] = rep
    if holes:
        lines = ["make_fold: no rule folds these characters:"]
        for ch, langs in sorted(holes, key=lambda p: ord(p[0])):
            lines.append("  U+%04X %s  (%s)  %s" % (
                ord(ch), ch, ", ".join(sorted(langs)),
                unicodedata.name(ch, "unnamed")))
        lines.append("Add them to LATIN, GREEK or CYRILLIC in tools/make_fold.py.")
        sys.exit("\n".join(lines))

    # Close the table under case: a name may be typed or shown either
    # way, and a lang file that only ever uses the small letter today
    # must not break the day one is capitalised.
    for ch in list(seen):
        for other in (ch.lower(), ch.upper()):
            if len(other) == 1 and ord(other) > 127 and ord(other) not in table:
                rep = fold_char(other)
                if rep is not None:
                    table[ord(other)] = rep

    longest = max((len(v) for v in table.values()), default=0)
    rows = []
    for cp in sorted(table):
        ch = chr(cp)
        rows.append('    {0x%04X, "%s"},  // %s %s' % (
            cp, table[cp], ch, unicodedata.name(ch, "unnamed").lower()))

    return ("""// GENERATED by tools/make_fold.py from lang/*.txt -- do not edit.
//
// Every character that occurs in any language the game ships, and the
// ASCII a player types to find it. See main/i18n/fold.h for why, and
// run `python3 tools/make_fold.py` after adding a language.
//
// %d characters; the longest folds to %d.

// A folded form is never longer than this, NUL included.
#define FOLD_REP_MAX %d

typedef struct {
    uint16_t   cp;
    char const rep[FOLD_REP_MAX];
} fold_entry_t;

// Sorted by codepoint: fold_known() and the fold itself binary-search it.
static fold_entry_t const FOLD_TABLE[] = {
%s
};
""" % (len(table), longest, longest + 1, "\n".join(rows)))


def main():
    text = build()
    if "--check" in sys.argv:
        try:
            with open(OUT, encoding="utf-8") as f:
                have = f.read()
        except FileNotFoundError:
            sys.exit("make_fold: %s is missing; run tools/make_fold.py" % OUT)
        if have != text:
            sys.exit("make_fold: %s is out of date; run tools/make_fold.py" % OUT)
        print("fold table up to date")
        return
    with open(OUT, "w", encoding="utf-8") as f:
        f.write(text)
    print("wrote %s" % os.path.relpath(OUT, HERE))


if __name__ == "__main__":
    main()
