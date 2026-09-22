# CraftMiner

A block world for the [Tanmatsu](https://nicolaielectronics.nl/), built on
[SynthEngine3D](https://github.com/nullislandspace/synthengine3D) and loaded by
[Graceloader](https://github.com/nullislandspace/tanmatsu-graceloader).

Slug `at.cavac.craftminer`. It installs to the **SD card only** — `metadata.json`
says `external_only`, so the launcher will not put it in internal flash, and
`make install` uploads to `/sd/apps/at.cavac.craftminer`.

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
Settings    Controls / Graphics / Audio / Display
Quit        back to the launcher
```

In a world, **Esc** (or whatever Pause is bound to) opens the pause menu —
**Resume / Save / Settings / Save and quit to title** — and **saves as it
opens**, since pausing is what people do before switching a handheld off.

* **Controls** — **Gyroscope** (off by default): look round by physically
  turning the badge; the cursor keys still work, so you choose when to move
  the badge and when to press keys. Below it, every action can be rebound: pick it, press the new key. A key
  another action already had moves to that action's old key, so no two share
  one. *Reset to defaults* puts them all back. Esc always pauses, whatever Pause
  is bound to, so there is no way to lock yourself out.
* **Graphics** — view distance (near / **medium**, the default / far), textures
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
file on the SD card, `/sd/apps/at.cavac.craftminer/settings.txt`, next to
`worlds/`. Copy that directory and you have backed up everything. The file is
plain `key=value` lines; delete one to get its default back.

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
| `F` | switch to the debug camera and back (not if you have bound F to something) |

The crosshair marks where the pick ray goes — which is **not** the centre of the
screen, because the engine's horizon row is 256 of 480. The block it finds gets
a wireframe box round it.

**Breaking a tree fells it.** That is deliberate and it is the project's one
declared departure from Minecraft: a log the world grew takes the whole tree
with it, a log *you placed* drops just itself. The difference is one bit in the
block's state byte. See `main/game/interact.h`.

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
```

Most of this game is portable C, on purpose: the world, generation, the mesher,
the codec, regions, the world store and the streaming loop all build with a
plain `cc` and are tested that way. The one seam is allocation
(`main/common/psram.h`), and `make hostpurity` fails the build if an engine or
RTOS header creeps into the pure set.

Worth knowing: **`make verify` cannot catch a missing symbol in code nothing
calls** — `--gc-sections` removes it from `app.so` first, so the check passes and
the app then fails to *load* on the badge. Anything working around a missing
graceloader export has to be exercised on the device, not merely compiled.

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
