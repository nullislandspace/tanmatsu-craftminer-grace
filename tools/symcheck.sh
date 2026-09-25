#!/usr/bin/env bash
# =====================================================================
#  SynthMiner  --  every symbol we call, the loader can resolve
# ---------------------------------------------------------------------
#  An ELF app for graceloader links against NOTHING: every libc and IDF
#  function it calls is resolved at LOAD time from the loader's export
#  table. A call to something that table does not carry therefore links
#  perfectly well, builds clean, uploads clean -- and then the app does
#  not start. No message, no console output, nothing on screen. It is
#  the single most expensive mistake available in this project, because
#  every symptom points somewhere else.
#
#  It happened with `strcasecmp` in main/audio/music.c (F-74), and
#  se_mp3.c's header records the same trap with `opendir`. So: after
#  every link, compare app.so's undefined symbols against the loader's
#  export list and name anything missing.
#
#  Needs ../tanmatsu-graceloader checked out. Without it this SKIPS
#  rather than fails, because a clone that only wants to build the app
#  should not need the loader's source -- but then nobody is protected,
#  which the skip message says out loud.
# =====================================================================
set -u

APP="${1:-build/app.so}"
EXPORTS="${GRACELOADER_EXPORTS:-../tanmatsu-graceloader/main/symbol_export/all}"

if [ ! -f "$APP" ]; then
    echo "symcheck: no $APP yet (build first)"
    exit 0
fi

if [ ! -f "$EXPORTS" ]; then
    echo "symcheck: SKIPPED -- $EXPORTS not found."
    echo "          Check out ../tanmatsu-graceloader to have unresolvable"
    echo "          symbols caught here instead of as an app that will not start."
    exit 0
fi

NM=""
for cand in riscv32-esp-elf-nm $(ls "$HOME"/idf/tools-*/tools/riscv32-esp-elf/*/riscv32-esp-elf/bin/riscv32-esp-elf-nm 2>/dev/null); do
    if command -v "$cand" >/dev/null 2>&1 || [ -x "$cand" ]; then NM="$cand"; break; fi
done
if [ -z "$NM" ]; then
    echo "symcheck: SKIPPED -- no riscv32-esp-elf-nm on PATH or under ~/idf"
    exit 0
fi

missing=0
while read -r sym; do
    [ -z "$sym" ] && continue
    if ! grep -qw -- "$sym" "$EXPORTS"; then
        if [ "$missing" -eq 0 ]; then
            echo "symcheck: the loader cannot resolve these, so the app WILL NOT START:"
        fi
        echo "  $sym"
        missing=$((missing + 1))
    fi
done < <("$NM" -u "$APP" | awk '{print $2}' | sort -u)

if [ "$missing" -gt 0 ]; then
    echo
    echo "  Each one is called by our code but is not in graceloader's export"
    echo "  table. Write it yourself (see ieq() in main/audio/music.c) or use"
    echo "  something that IS exported -- do not add it to the loader unless"
    echo "  that is really the intent."
    exit 1
fi

echo "symcheck: every undefined symbol is exported by graceloader"
exit 0
