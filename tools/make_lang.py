#!/usr/bin/env python3
"""Bake lang/*.txt into main/i18n/strings_gen.{h,c}.

    python3 tools/make_lang.py          # regenerate
    python3 tools/make_lang.py --check  # fail if the generated files are stale

Normally nobody runs either by hand: the Makefile has a rule that regenerates
the tables whenever a lang file or this script changes, so editing a
translation and building is enough. --check is the CI form, which asks whether
what is COMMITTED is up to date instead of bringing it up to date.

`lang/en.txt` defines the keys and the English text. Every other language is
held against it:

  * a key English does not have is an error (a typo, or a key that was renamed
    and the translation not);
  * a key a translation is missing falls back to the English text, here, so
    the generated arrays are full and the game never has to check;
  * the %-placeholders must be the same ones, in some order. A translation may
    move them with `%2$s` and pad them differently, but it cannot change what
    they are, drop one, or invent one -- at run time i18n_fmt() takes the
    types from English regardless, so a mismatch here is a mistake the
    translator wants to hear about rather than a crash.

The generated files are checked in, so a clone that only compiles needs no
Python -- and so that an edited lang file cannot be committed without the
tables that came from it, which is what `make langcheck` guards in CI.
"""

import os
import re
import sys
import unicodedata

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
LANG_DIR = os.path.join(ROOT, "lang")
OUT_H = os.path.join(ROOT, "main", "i18n", "strings_gen.h")
OUT_C = os.path.join(ROOT, "main", "i18n", "strings_gen.c")

MAX_ARGS = 6  # keep in step with I18N_FMT_MAX_ARGS (i18n.h)

# The languages, in the order they appear in the menu, with the name each one
# calls itself. That name is NEVER translated: a player who cannot read the
# language the game is currently in has to be able to find their own.
LANGUAGES = [
    # English first -- it is the reference, and the one a player who cannot
    # read the current setting is most likely to recognise. The rest are
    # alphabetical BY THE NAME SHOWN, which is the name a player is looking
    # for; accents are ignored when sorting, so Čeština sits under C and
    # Íslenska under I. Greek and Cyrillic follow the Latin names, each in
    # its own alphabet's order.
    ("en", "SM_LANG_EN", "English"),
    ("ca", "SM_LANG_CA", "Català"),
    ("cs", "SM_LANG_CS", "Čeština"),
    ("da", "SM_LANG_DA", "Dansk"),
    ("de", "SM_LANG_DE", "Deutsch"),
    ("et", "SM_LANG_ET", "Eesti"),
    ("es", "SM_LANG_ES", "Español"),
    ("fr", "SM_LANG_FR", "Français"),
    ("ga", "SM_LANG_GA", "Gaeilge"),
    ("hr", "SM_LANG_HR", "Hrvatski"),
    ("is", "SM_LANG_IS", "Íslenska"),
    ("it", "SM_LANG_IT", "Italiano"),
    ("lv", "SM_LANG_LV", "Latviešu"),
    ("lt", "SM_LANG_LT", "Lietuvių"),
    ("hu", "SM_LANG_HU", "Magyar"),
    ("nl", "SM_LANG_NL", "Nederlands"),
    ("no", "SM_LANG_NO", "Norsk"),
    ("pl", "SM_LANG_PL", "Polski"),
    ("pt", "SM_LANG_PT", "Português"),
    ("ro", "SM_LANG_RO", "Română"),
    ("sq", "SM_LANG_SQ", "Shqip"),
    ("sk", "SM_LANG_SK", "Slovenčina"),
    ("sl", "SM_LANG_SL", "Slovenščina"),
    ("fi", "SM_LANG_FI", "Suomi"),
    ("sv", "SM_LANG_SV", "Svenska"),
    ("tr", "SM_LANG_TR", "Türkçe"),
    ("nl-BE", "SM_LANG_NL_BE", "Vlaams"),
    ("el", "SM_LANG_EL", "Ελληνικά"),
    ("bg", "SM_LANG_BG", "Български"),
    ("ru", "SM_LANG_RU", "Русский"),
    ("sr", "SM_LANG_SR", "Српски"),
    ("uk", "SM_LANG_UK", "Українська"),
]

SPEC_RE = re.compile(r"%(?:(\d+)\$)?([-+ #0]*)(\d*)(?:\.(\d+))?(hh|h|ll|l|z|j|t)?([diuxXofFeEgGsc%])")


def parse_file(path):
    """[(key, text)], in file order. Raises on a malformed line."""
    out, seen = [], set()
    with open(path, "r", encoding="utf-8") as f:
        for lineno, raw in enumerate(f, 1):
            line = raw.split("#", 1)[0].strip() if raw.lstrip().startswith("#") else raw
            line = line.rstrip("\n")
            if not line.strip() or line.lstrip().startswith("#"):
                continue
            if "=" not in line:
                sys.exit("%s:%d: no '=' in %r" % (os.path.basename(path), lineno, line.strip()))
            key, text = line.split("=", 1)
            key, text = key.strip(), text.strip()
            if not key:
                sys.exit("%s:%d: empty key" % (os.path.basename(path), lineno))
            if key in seen:
                sys.exit("%s:%d: %s appears twice" % (os.path.basename(path), lineno, key))
            seen.add(key)
            out.append((key, text))
    return out


def specs(text):
    """The %-placeholders of a string: [(index-or-None, conversion)], in the
    order they are written."""
    return [(int(m.group(1)) - 1 if m.group(1) else None, m.group(6))
            for m in SPEC_RE.finditer(text) if m.group(6) != "%"]


def check_specs(key, english, other, code):
    """Same placeholders, whatever their order."""
    want = [conv for _, conv in specs(english)]
    if len(want) > MAX_ARGS:
        sys.exit("en.txt: %s takes %d values, and I18N_FMT_MAX_ARGS is %d"
                 % (key, len(want), MAX_ARGS))
    got = specs(other)
    used = []
    for pos, (idx, conv) in enumerate(got):
        which = pos if idx is None else idx
        if which >= len(want):
            sys.exit("%s.txt: %s uses %%%d$, but English has only %d value%s"
                     % (code, key, which + 1, len(want), "" if len(want) == 1 else "s"))
        if conv != want[which]:
            sys.exit("%s.txt: %s has %%%s where English has %%%s (value %d). The type comes "
                     "from English, so this would print the wrong thing."
                     % (code, key, conv, want[which], which + 1))
        used.append(which)
    for i in range(len(want)):
        if i not in used:
            sys.exit("%s.txt: %s leaves out value %d (%%%s), which English fills in"
                     % (code, key, i + 1, want[i]))


def check_scripts(key, text, code):
    """A word with two alphabets in it.

    This is the typo a machine translation makes and a human never notices:
    a Latin `a` inside a Cyrillic word looks identical and reads as a box on
    a badge with no Latin-Cyrillic lookalike merging. The game's own name is
    the one honest exception -- Serbian declines it as `SynthMiner-ом`."""
    def script(ch):
        n = unicodedata.name(ch, "")
        for s in ("CYRILLIC", "GREEK", "LATIN"):
            if n.startswith(s):
                return s
        return None

    for word in re.findall(r"\S+", text):
        if "SynthMiner" in word:
            continue
        scripts = {script(c) for c in word if script(c)}
        if len(scripts) > 1:
            sys.exit("%s.txt: %s: %r mixes %s. One of those letters is not the "
                     "one it looks like." % (code, key, word, " and ".join(sorted(scripts))))


def c_string(s):
    out = []
    for ch in s:
        if ch == '"':
            out.append('\\"')
        elif ch == "\\":
            out.append("\\\\")
        elif ch == "\n":
            out.append("\\n")
        elif ord(ch) < 32:
            out.append("\\%03o" % ord(ch))
        else:
            out.append(ch)  # UTF-8 goes straight through: the font reads it
    return '"' + "".join(out) + '"'


def enum_name(key):
    return "SM_STR_" + re.sub(r"[^A-Z0-9]", "_", key.upper())


def build():
    english = parse_file(os.path.join(LANG_DIR, "en.txt"))
    keys = [k for k, _ in english]
    en_text = dict(english)

    names = {}
    for k in keys:
        n = enum_name(k)
        if n in names:
            sys.exit("%s and %s both become %s" % (names[n], k, n))
        names[n] = k

    tables, notes = {}, []
    for code, _, _ in LANGUAGES:
        path = os.path.join(LANG_DIR, "%s.txt" % code)
        if code == "en":
            tables[code] = en_text
            continue
        if not os.path.exists(path):
            sys.exit("lang/%s.txt is missing" % code)
        pairs = dict(parse_file(path))
        for k in pairs:
            if k not in en_text:
                sys.exit("lang/%s.txt: %s is not a key in en.txt" % (code, k))
        missing = [k for k in keys if k not in pairs]
        if missing:
            notes.append("%s: %d of %d untranslated, showing English"
                         % (code, len(missing), len(keys)))
        for k in keys:
            if k in pairs:
                check_specs(k, en_text[k], pairs[k], code)
                check_scripts(k, pairs[k], code)
            else:
                pairs[k] = en_text[k]
        tables[code] = pairs
    return keys, names, tables, notes


BANNER = """// GENERATED by tools/make_lang.py from lang/*.txt -- do not edit by hand.
// Edit the text there and run `python3 tools/make_lang.py`; `make check`
// fails if these files are older than the lang files they came from.
"""


def render():
    keys, names, tables, notes = build()
    order = {k: enum_name(k) for k in keys}

    h = [BANNER, """// =====================================================================
//  SynthMiner  --  every string the UI can show, and every language
// ---------------------------------------------------------------------
//  One enum for the strings, one for the languages, and a table of
//  pointers per language. A lookup is an array index; there is nothing
//  to parse and nothing to allocate. See i18n.h.
// =====================================================================

#ifndef SM_STRINGS_GEN_H
#define SM_STRINGS_GEN_H

typedef enum {"""]
    for k in keys:
        h.append("    %-34s  // %s" % (order[k] + ",", k))
    h.append("    SM_STR_COUNT")
    h.append("} sm_str_t;\n")
    h.append("typedef enum {")
    for code, name, autonym in LANGUAGES:
        h.append("    %-16s  // %-6s %s" % (name + ",", code, autonym))
    h.append("    SM_LANG_COUNT")
    h.append("} sm_lang_t;\n")
    h.append("// [language][string]. Full for every language: a translation that")
    h.append("// does not have a string was given the English one when this was")
    h.append("// generated, so nothing has to fall back at run time.")
    h.append("extern char const* const SM_STRINGS[SM_LANG_COUNT][SM_STR_COUNT];")
    h.append("extern char const* const SM_LANG_CODES[SM_LANG_COUNT];")
    h.append("extern char const* const SM_LANG_NAMES[SM_LANG_COUNT];")
    h.append("// The key each string is known by in lang/*.txt, for the override")
    h.append("// files a player may put on the SD card (i18n_load_overrides).")
    h.append("extern char const* const SM_STR_KEYS[SM_STR_COUNT];")
    h.append("\n#endif  // SM_STRINGS_GEN_H")

    c = [BANNER, '#include "i18n/strings_gen.h"\n']
    c.append("char const* const SM_LANG_CODES[SM_LANG_COUNT] = {")
    c.append("    " + " ".join('%s,' % c_string(code) for code, _, _ in LANGUAGES))
    c.append("};\n")
    c.append("char const* const SM_LANG_NAMES[SM_LANG_COUNT] = {")
    c.append("    " + " ".join('%s,' % c_string(a) for _, _, a in LANGUAGES))
    c.append("};\n")
    c.append("char const* const SM_STR_KEYS[SM_STR_COUNT] = {")
    for k in keys:
        c.append("    %s," % c_string(k))
    c.append("};\n")
    c.append("char const* const SM_STRINGS[SM_LANG_COUNT][SM_STR_COUNT] = {")
    for code, name, autonym in LANGUAGES:
        c.append("    [%s] = {  // %s" % (name, autonym))
        for k in keys:
            c.append("        [%s] = %s," % (order[k], c_string(tables[code][k])))
        c.append("    },")
    c.append("};")

    return "\n".join(h) + "\n", "\n".join(c) + "\n", keys, notes


def main():
    text_h, text_c, keys, notes = render()
    check = "--check" in sys.argv
    stale = []
    for path, text in ((OUT_H, text_h), (OUT_C, text_c)):
        old = open(path, encoding="utf-8").read() if os.path.exists(path) else None
        if old == text:
            continue
        if check:
            stale.append(os.path.relpath(path, ROOT))
        else:
            os.makedirs(os.path.dirname(path), exist_ok=True)
            open(path, "w", encoding="utf-8").write(text)
    if check and stale:
        sys.exit("out of date with lang/*.txt: %s\n  run: python3 tools/make_lang.py"
                 % ", ".join(stale))
    for n in notes:
        print("  " + n)
    print("  %d strings x %d languages" % (len(keys), len(LANGUAGES)))


if __name__ == "__main__":
    main()
