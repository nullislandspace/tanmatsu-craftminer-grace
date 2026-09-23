# CraftMiner

A block world for the [Tanmatsu](https://nicolaielectronics.nl/), built on
[SynthEngine3D](https://github.com/nullislandspace/synthengine3D) and loaded by
[Graceloader](https://github.com/nullislandspace/tanmatsu-graceloader).

Slug `at.cavac.craftminer`. It installs to the **SD card only** — `metadata.json`
says `external_only`, so the launcher will not put it in internal flash, and
`make install` uploads to `/sd/apps/at.cavac.craftminer`.

**The player's data is NOT there.** Worlds, `settings.txt`, replays and
screenshots live in **`/sd/craftminer`**, which the launcher does not manage,
so an update or a reinstall cannot delete them. The install directory holds
only what the app ships with (`app.so`, the textures). Builds before this kept
the data in the install directory; the first start of a newer one moves it
across (`main/world/datadir.h`).

The project comes from
[tanmatsu-template-grace](https://github.com/nullislandspace/tanmatsu-template-grace),
which stays as the `upstream` remote: `git fetch upstream && git merge upstream/main`
brings in graceloader's symbol-export updates. Its facilities are documented below.

```sh
git clone --recursive git@github.com:nullislandspace/tanmatsu-craftminer-grace.git
make badgelink     # once: the flashing/file-transfer tools
make build         # app.so
make install run   # onto the SD card, then start it
```

## 3D: SynthEngine3D

[SynthEngine3D](https://github.com/nullislandspace/synthengine3D) is the 3D engine for
graceloader apps: a software rasteriser (z-buffer and raycast), PPA compositing, meshes,
textures, lighting, audio and UI helpers, with its own `se_run()` main loop.

The engine is **not** shipped with the template, so apps that do not want it are not
carrying it around. What the template does ship is the build wiring, which sits idle until
an app adds the engine. In a new app that wants 3D:

```sh
make engine     # git submodule add -b main git@github.com:nullislandspace/synthengine3D.git synthengine3D
git add .gitmodules synthengine3D && git commit -m "Add SynthEngine3D"
```

`CMakeLists.txt` picks it up by itself: when `synthengine3D/CMakeLists.txt` exists it builds
the engine, propagates its include directory (so app sources can `#include "synthengine3d.h"`)
and folds its objects into `app.so`. When it does not, the link line is exactly the plain
one, which is why every app can keep this template as `upstream` whether it uses 3D or not.

Two things follow from it being a submodule:

* clone such an app with `git clone --recursive`, or run `git submodule update --init` in it;
* `git submodule update --remote synthengine3D` moves it to the newest engine, and an app
  that wants a fixed version pins it (`ENGINE_REF=V2.0 make engine`, or check out the tag
  inside `synthengine3D/` and commit the new pointer).

Engine settings (list caps and the like) are compile definitions that must reach the
`synthengine3d` target, so set them with `add_compile_definitions()` **before**
`add_subdirectory(synthengine3D)` — see the engine's `docs/configuration.md`.

## Playing it

It opens on the engine's splash, then the title: **CraftMiner** written in real
blocks in a real world, over a meadow the generator made, with **Play /
Settings / Quit** underneath.

### Menus

Cursor keys choose, **Enter** selects, **Esc** goes back.

```
Play        eight save slots, each empty or holding a named world
  a world     Play / Rename / Delete (asks first)
  empty       New world: name, seed (a number, any text, or blank for random), Create
Settings    Language / Controls / Graphics / Audio / Display
Quit        back to the launcher
```

In a world, **Esc** (or whatever Pause is bound to) opens the pause menu —
**Resume / Save / Settings / Save and quit to title** — and **saves as it
opens**, since pausing is what people do before switching a handheld off.

* **Language** — 32 of them, listed under "Languages" below. First row of
  Settings, and each stands under its own name, so the player who needs that
  row is not asked to read a language they do not have. Takes effect at once
  and is remembered in `settings.txt`.
* **Controls** — **Gyroscope** (off by default): look round by physically
  turning the badge; the cursor keys still work, so you choose when to move
  the badge and when to press keys. Below it, every action can be rebound: pick it, press the new key. A key
  another action already had moves to that action's old key, so no two share
  one. *Reset to defaults* puts them all back. Esc always pauses, whatever Pause
  is bound to, so there is no way to lock yourself out.
* **Graphics** — view distance (**near**, the default / medium / far), textures
  on or off, half or full resolution, clouds on or off, and the **camera**:
  first person (Fred's arm and what it holds) or third person (Fred himself,
  the camera behind him), and **Fred's hand**: right (the default) or left, in
  both views.
* **Audio** — the device volume, plus music and sound-effect switches that are
  remembered for when the game has sounds (it has none yet).
* **Display** — screen, keyboard and LED brightness.

Volume and brightness are the device's own settings, shared with the launcher
and every other app (`se_hw`, the launcher's `system` NVS namespace). Everything
else — graphics, audio switches, gyroscope, every key binding — is one text
file on the SD card, `/sd/craftminer/settings.txt`, next to `worlds/`,
`replays/` and `screenshots/`. Copy that directory and you have backed up
everything. The file is
plain `key=value` lines; delete one to get its default back.

### Languages

CraftMiner speaks **32 languages**. English is the reference; the other 31 are
a **machine's work**, and every one of them says so at the top of its file.
They have not been read by anyone who speaks them. **That is where you come in
— see "Fixing a translation" below.**

```
English    Català     Čeština    Dansk      Deutsch    Eesti      Español
Français   Gaeilge    Hrvatski   Íslenska   Italiano   Latviešu   Lietuvių
Magyar     Nederlands Norsk      Polski     Português  Română     Shqip
Slovenčina Slovenščina Suomi     Svenska    Türkçe     Vlaams
Ελληνικά   Български  Русский    Српски     Українська
```

The menu lists them with English first and the rest alphabetically by the name
each language calls itself — the name you are looking for is the one you can
read. Settings → Language, two rows in, and it takes effect at once.

Not translated, deliberately: the name CraftMiner, world names (you type
those), the key names in the Controls list — `Esc`, `Space`, `Left Shift` are
what is printed on the badge's own keys — and every log line.

### Fixing a translation

**You do not need a toolchain, a compiler, or a GitHub account.** Put a file at
`/sd/craftminer/lang/<code>.txt` on the SD card — `de.txt`, `nl-BE.txt`,
`bg.txt` — holding just the lines you want changed:

```
menu.play = Spelen
status.deleted = Wereld gewist
```

Those win over what is built into the game; every other string stays as it
shipped. One line in the file changes one string. Restart the app, or switch
the language away and back, and you will see it.

**To fix it for everybody**, send a pull request:

1. Edit `lang/<code>.txt` in this repository. Keys come from `lang/en.txt`,
   which is the reference — leave the key alone and change the text after `=`.
2. Build. `make` regenerates `main/i18n/strings_gen.*` from the lang files
   whenever they change — you do not have to run anything by hand — and the
   generated files are committed, so **commit those too**, alongside your lang
   file. (`make lang` on its own does just that step.)
3. Run `make check`. It fails on a key that is not in `lang/en.txt`, on
   `%`-placeholders that do not match English's, on a word that mixes two
   alphabets (a Latin `a` inside a Cyrillic word looks identical and is not),
   and on any character the font cannot draw.
4. Open a pull request. **Say which language you actually speak** — that is
   the whole point, and it is the one thing the checks cannot verify.

Small corrections are as welcome as whole files: one clumsy sentence fixed by
someone who would never have written it that way is worth more than a hundred
strings nobody has read.

**A whole new language** needs a `lang/<code>.txt` (copy `en.txt` and
translate), a row in `tools/make_lang.py`, and — if it needs letters the font
has never drawn — a pass through `synthengine3D/tools/hershey/README.md`, which
covers that end and will tell you exactly which letters are missing.

### How the text works

The strings live in `lang/*.txt`, plain `key = text` lines in UTF-8, and
`tools/make_lang.py` bakes them into `main/i18n/strings_gen.c`, which is what
ships. A lookup at run time is an array index — nothing is parsed and nothing
allocated while the game runs.

That baking is an ordinary make rule: edit a lang file and the next `make`
regenerates the tables, the way `.o` files follow their `.c`. The generated
files are committed all the same, so a clone that only compiles needs no
Python; `make langcheck` is the CI form, which asks whether what is committed
is already up to date rather than bringing it up to date.

A translation may reorder the values in a line (`%2$s` before `%1$d`) where the
language needs a different word order. It cannot change what a value *is*: the
types come from English, so a lang file on a card can be wrong without being
dangerous.

### Day, night and light

A day is 20 minutes, and the world's clock only runs while you play it. The
sun rises in the east, the sky turns orange at sunrise and sunset and dark
blue at night, with a square moon, stars and drifting blocky clouds.

**Torches light the area round them** — fourteen blocks, one level dimmer a
block, through air, glass and plants, dimmed by leaves and water, stopped by
anything solid. Daylight comes down through open sky and a little way into
cave mouths; a cave with no torch in it is dark at noon. Light is worked out
when a block is placed or removed, never per frame.

### What a save keeps

Where you were — exactly, so a cave is still a cave when you come back —
which way you faced, health, hunger, **everything you carry**, down to each
tool's wear, **everything lying on the ground**, and the world's time of day. Items are stored by name, so the inventory survives items being
added or renumbered. Edited terrain is saved with it. A world is written when
you pause, save, quit to the title, or when a chunk you edited leaves memory;
never on a timer.

**The Testworld.** Builds before save slots kept a single world. The first
time this build starts it moves that world into slot 1 and calls it
*Testworld*, terrain and player intact. On a card that never had one there is
nothing to move.

### Keys in a world

The camera is the player unless you press **F**. The defaults:

| key | |
|---|---|
| `W` `A` `S` `D` | walk |
| cursor keys | look |
| `Space` | jump |
| `L-Shift` | sneak |
| `Q` | **hold** to break the block under the crosshair |
| `E` | place the selected one |
| `G` | drop what you are holding |
| `Tab` | inventory — cursor keys move, `F1`–`F6` put a stack on the hotbar |
| `F1`–`F6` | hotbar slot |
| `Esc` | pause menu |
| `Backspace` | show position, heading and time of day |
| `0` | screenshot, saved to `/sd/craftminer/screenshots/shotNNN.png` |
| `F` | switch to the debug camera and back (not if you have bound F to something) |

The crosshair marks where the pick ray goes — which is **not** the centre of the
screen, because the engine's horizon row is 256 of 480. The block it finds gets
a wireframe box round it.

**Breaking a tree fells it.** That is deliberate and it is the project's one
declared departure from Minecraft: a log the world grew takes the whole tree
with it, a log *you placed* drops just itself. The difference is one bit in the
block's state byte. See `main/game/interact.h`.

## Sound

**Settings → Audio** has three sliders and two switches, and the sliders are
not the same kind of thing:

| | |
|---|---|
| **Volume** | the *badge's* volume, the launcher's setting, shared with every app. Turning it down quietens everything. |
| **Music** / **Music volume** | on-off, and how loudly the music is mixed into this game |
| **Effects** / **Effects volume** | the same for footsteps, breaking and the rest |

So you can leave the badge where it is and just push the music back behind the
footsteps. The two game sliders live in `settings.txt`; the device volume does
not, because a game keeping its own copy of a device setting is a game that
disagrees with the device.

The speaker is held powered for as long as the game runs
(`audio_mixer_keep_awake`, engine 2.2). That is not gratuitous: the mixer
otherwise mutes the amplifier after ~46 ms of quiet, and an amplifier's
turn-on is slow enough to swallow a 35 ms tool click whole — which showed up
as "the tool sounds only play when the music is on", because an installed
music source keeps the mixer busy whether it is making a sound or not.

### Effects

Every sound is a row in a table (`main/audio/sfx.c`): a tone layer and a noise
layer through one filter and one envelope, with a little pitch jitter on every
play so no two footsteps are identical. Nothing is sampled, so the whole set
costs about two kilobytes of constants.

**Which sound a block makes is the block's own business.** `block_def_t.sound`
names a material class — `SND_STONE`, `SND_WOOD`, `SND_GRAVEL`, `SND_SAND`,
`SND_GLASS`, `SND_SOFT`, `SND_SPLASH` — and the footstep, the break and the
place all follow from it. Adding a block therefore adds its sounds in the same
row, with no edit in the audio code. Footsteps are counted by **distance
walked**, not by ticks, so a slow walk does not machine-gun and being pushed
along sounds different from walking.

### Music

Eleven pieces of out-of-copyright classical music, played the way the Minecraft
betas did it: a piece starts, it ends, and then there is nothing for several
minutes. The next one is chosen at random — never in playlist order, and never
the same piece twice running — and the gap is random too.

They are **Standard MIDI Files**, played by a sequencer (`main/audio/midi_seq.c`)
through a small synthesiser (`main/audio/midi_synth.c`). All eleven together are
72 KB. The same music as MP3 would be megabytes, and would need a decoder task
with a 32 KB stack; this needs neither.

It is not a General MIDI sound module and does not pretend to be. The 128 GM
programs collapse onto six voice shapes — a struck string, a plucked one, a
sustained pad, a bass, a reed and a bell — chosen so the piano repertoire comes
out recognisable. A file that leans on one specific patch will sound like
something else.

### Your own music

Drop any `.mid` file into `/sd/craftminer/music/` and it joins the pool. A file
there with the same name as one of ours replaces it, so you can swap an
arrangement you do not like without deleting anything. Nothing needs rebuilding,
and no toolchain is involved — the same arrangement the translations use.

### Where the music came from, and the catch

A piece of music has **two** copyrights and both have to be clear:

1. **the composition** — Satie, Debussy, Chopin, Schumann and Bach are all long
   out of copyright everywhere;
2. **the engraving** — the particular typeset edition a MIDI file was generated
   from is a new work with its own copyright, *even when the music in it is
   ancient*. This is the one that catches people out. A MIDI file found loose on
   the web is almost never licensed for redistribution, whoever wrote the tune.

So every file comes from the [Mutopia Project](https://www.mutopiaproject.org/),
and **only from its Public Domain set**, never its Creative Commons one — CC
BY-SA would have put share-alike conditions on anyone redistributing the game.
`assets/music/MUSIC.md` records where each file came from, and
`tools/get_music.py` refuses to download anything that is not Public Domain, so
extending the set cannot go wrong by accident:

```
python3 tools/get_music.py            # check the shipped files against Mutopia
python3 tools/get_music.py --survey   # what else is available, with licences
```

`make check` parses every file in `assets/music` with the real sequencer: that
it loads, that it has notes in it, that it **ends**, that rewinding replays it
identically, and that every truncation of it still terminates.

`make check` also measures every settings-row label, in every language,
against the value column it sits beside — with the engine's own glyph advances
at the menu's row height, so a label that passes fits on the badge. That check
exists because three screens turned out to be overlapping their own text in a
dozen languages: "every character is drawable" and "every label fits" look
like the same question and are not.

## Flying it by hand

**F** in a world swaps the player for a free camera and back. During a `perf` or
`shots` test the camera follows a scripted path instead, because a reproducible
frame cannot depend on which keys are held.

| key | |
|---|---|
| `W` `A` `S` `D` | move horizontally, along where you are looking |
| `Space` / `L-Shift` | up / down |
| cursor keys | look |
| `L-Ctrl` | three times the speed |
| `P` | pause the scripted flight |
| `R` | in a world: start / stop recording a replay (`replays/last.cmr`) |
| `N` | in a world: the clock a quarter of a day on (morning, noon, evening, midnight) |

Textures and view distance, which used to be the `T` and `V` keys, are in
Settings → Graphics.

## Where the frame time goes

The app logs a memory map and a memory benchmark at boot
(`main/game/membench.c`), and the engine reports pixels **and spans** per
rasterize pass (`scene_fill_stats`). Between them, `make cycle
TEST="perf scene=block secs=20"` says whether a fill loop is bound on its
arithmetic, on memory, or on its own setup — the three are indistinguishable
from a frame rate alone, and two of the three were guessed wrong here before
they were measured. See `claudeplans/craftminer.md`, F-40.

Short version, on this hardware: spans average **six pixels**, so per-span
setup dominates, and vectorising the inner loops (the ESP32-P4's PIE SIMD,
which this toolchain already enables) would attack the cheapest part.

## The world, and how to work on it

**Block ids are permanent.** Worlds store blocks as one-byte ids, so a
block's id and name never change once shipped, and blocks are retired, never
removed; item names likewise. `tools/ids.txt` lists every one, and `make
check` fails the build if the code disagrees with it -- adding a block means
appending a line there.

`claudeplans/craftminer.md` is the living plan: the design, a step-by-step
status table, and the findings and decisions logs. Read it first — every number
quoted below comes from a measurement recorded there.

```
main/common/    the host/badge seam, seeded noise, tagged fields
main/math/      vectors, meshes, the camera          (lifted from the showreel)
main/voxel/     the greedy mesher, sky, effects      (lifted from the showreel)
main/world/     blocks, chunks, generation, saving, streaming, rendering
tools/          host checks and the badge helpers
textures/       20 generated 16x16 block textures
```

### Host checks — seconds, no badge

```sh
make check          # hostpurity + meshcheck + worldcheck; `make build` needs it
make symcheck       # every symbol we call, the loader can resolve (after the link)
```

Most of this game is portable C, on purpose: the world, generation, the mesher,
the codec, regions, the world store and the streaming loop all build with a
plain `cc` and are tested that way. The one seam is allocation
(`main/common/psram.h`), and `make hostpurity` fails the build if an engine or
RTOS header creeps into the pure set.

**Unresolvable symbols.** An ELF app for graceloader links against nothing:
every libc and IDF function is resolved at *load* time from the loader's export
table. A call to something that table does not carry compiles clean, links
clean, uploads clean — and then the app does not start, with no message and no
console. `make build` now runs `tools/symcheck.sh` after the link, which
compares `app.so`'s undefined symbols against
`../tanmatsu-graceloader/main/symbol_export/all` and names anything missing.
(It skips, loudly, if that checkout is not there.) `strcasecmp` is one such
symbol; `opendir` is another.

The remaining gap: **a missing symbol in code nothing calls** still slips
through, because `--gc-sections` removes it from `app.so` before either check
can see it. Anything working around a missing export has to be *exercised* on
the device, not merely compiled.

### Talking to the badge

```sh
make ping           # does the app answer, and which build is it running?
make mode           # put the badge in BadgeLink mode (probes first)
make exitapp        # ask a running app to return to the launcher
```

These are thin wrappers over `tools/testrun.py`'s own connection code. Reach for
`make ping` when a cycle fails and it is not clear whether the app is alive,
wedged, or never started — a stale app holding the USB link and a bridge that is
down look identical to every other tool.

## Automated device tests

`main/testkit/` is a ready-made test loop for an app on real hardware. The host
sends a command over the debug console, the app runs it inside its own frame
loop, reports machine-readable records, and returns to the launcher by itself —
so `make cycle` builds, installs, runs, tests and comes back with a verdict
without anyone touching the badge.

```sh
make cycle       TEST="perf  scene=title secs=20"       # frame rate, phase split, primitive counts
make cycle       TEST="shots scene=title ms=0,1500,4000" # render exact instants, save PNGs + hashes
make testrefs    TEST="shots scene=title ms=0,1500,4000" # store those hashes as the references
make testcompare TEST="shots scene=title ms=0,1500,4000" # compare against them: a regression test
make recover                                             # after a crash or a hang
```

Results land in `results/<UTC>-<test>-<scene>/` as `console.log` + `result.json`;
references live in `tests/refs/manifest.json`. Exit codes: 0 ok, 1 link, 2 crash,
3 the test reported bad, 4 an image mismatch, 5 usage.

### What it is

| File | What |
|---|---|
| `testkit/debugcon.*` | The console listener: `PING`, `RUN <test> k=v`, `EXIT`, `BADGELINK`, read through the USB-serial/JTAG **driver** (graceloader 2.4.0+ exports it). Emits a `READY` record every 2 s while idle, so the host can find the app without sending anything. |
| `testkit/report.*` | The record format: `@@SR-<KIND>@@ <json> @@<crc32>@@`, one line, CRC'd so a line another task interleaved into it is dropped rather than believed. |
| `testkit/devtest.*` | The two tests (`perf`, `shots`) and the runner that drives them. |
| `testkit/profile.*` | Per-phase frame timing (`prof_begin`/`prof_end`), reported in each `PERF` record. The phase names are yours. |
| `testkit/screenshot.*` | Framebuffer → PNG on the SD card, with a deflate *stored* stream so it needs 64 KB of PSRAM rather than a ~130 KB compressor in scarce internal RAM. |
| `testkit/showtime.*` | The clock everything hangs off: real time, or fixed steps of 1/fps, or set outright. |
| `tools/testrun.py` | The host side: connect, identify, refuse a stale build, run, collect, compare, write results. |
| `tools/recover.py` | Get a wedged badge back. |

### Wiring it into an app

1. Add `main/testkit/*.c` to `APP_SOURCES`, and `main` to `APP_INCLUDES` (it is
   there already). Set the app's paths and name while you are there:

   ```cmake
   add_compile_definitions(SCREENSHOT_DIR="/sd/myapp")
   # REPORT_PREFIX="SR" by default; change it only if two apps' logs mix,
   # and pass the same to testrun.py with --prefix.
   ```

2. Tell the kit how to address your content — one struct, five functions:

   ```c
   static devtest_content_t const CONTENT = {
       .select    = level_select,     // play this one from its start; false if unknown
       .duration  = level_duration,   // seconds, <= 0 for endless
       .started   = level_started,    // show time at which it began
       .name      = level_name,       // what is selected now
       .shot_name = level_section,    // sub-section for per-shot stats, or ""
   };
   static devtest_config_t const TEST = {
       .app = "tld.username.myapp", .shot_dir = "/sd/myapp/test", .content = &CONTENT,
   };
   ```

3. Call it from the frame loop:

   ```c
   on_init:    devtest_start(&TEST);
   on_update:  showtime_frame(); devtest_update(); /* then advance your own content */
   on_render:  /* draw the frame */ devtest_after_render(fb, rast_us);
   per second: devtest_period(fps, frame_ms);
   ```

An app without SynthEngine3D compiles the kit with `TESTKIT_NO_ENGINE`: it then
reports timings and heap, and the primitive counts read zero.

### The one precondition

The `shots` test renders *chosen instants*: it sets the clock instead of running
it. That is only meaningful if what you draw is a **pure function of
`showtime_now()`** — same t, same picture. Get that right and a stored hash is a
real regression test, a host-side checker can replay the same instant (see the
engine's `docs/testing.md`), and a video export can render far slower than real
time without changing a frame. An app that accumulates per-frame `dt`, or draws
from unseeded randomness, can still use `perf`, but its shot hashes will wobble
and the references mean nothing.

## License

This software is under the [MIT license](https://opensource.org/license/mit). The MIT license allows others to build upon your work without restrictions while also making sure you retain your attribution.

(C) 2026 Rene Schickbauer

The music in `assets/music` is **not** covered by that licence and does not need
to be: every file is in the **public domain**, both as a composition and as the
particular engraving it was made from. `assets/music/MUSIC.md` names the source
of each one. Nothing in that directory places any obligation on the rest of the
game, which is exactly why the Creative Commons files were left on the shelf.
