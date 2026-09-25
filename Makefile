PORT ?= /dev/ttyACM0
BADGELINKPORT ?= $(PORT)

SHELL := /usr/bin/env bash

# App installation settings
APP_SLUG_NAME ?= at.cavac.synthminer
# The SD card, not internal flash: metadata.json says external_only,
# so a launcher install goes here too.
APP_INSTALL_BASE_PATH ?= /sd/apps/
APP_INSTALL_PATH = $(APP_INSTALL_BASE_PATH)$(APP_SLUG_NAME)

# ESP-IDF tools path (needed for the RISC-V cross-compiler)
IDF_PATH ?= $(shell cat .IDF_PATH 2>/dev/null || echo `pwd`/esp-idf)
IDF_TOOLS_PATH ?= $(shell cat .IDF_TOOLS_PATH 2>/dev/null || echo `pwd`/esp-idf-tools)
IDF_VERSION ?= v6.0.2
IDF_GITHUB_ASSETS ?= dl.espressif.com/github_assets

# The ESP-IDF environment script. Only mode_badgelink needs it (for pyserial
# out of the IDF python env -- the app build itself just needs the toolchain
# via IDF_TOOLS_PATH). Honours an IDF_SOURCE already exported by the shell.
IDF_SOURCE ?= $(shell cat .IDF_PATH 2>/dev/null && echo '$(IDF_PATH)/export.sh' || test -d `pwd`/esp-idf && echo '$(IDF_PATH)/export.sh' || echo '$(HOME)/.espressif/tools/activate_idf_$(IDF_VERSION).sh')

export IDF_TOOLS_PATH
export IDF_GITHUB_ASSETS

BUILD ?= build

# The block textures. Generated (tools/make_textures.py) and committed, so a
# clone builds without numpy; `make textures` regenerates them byte-identically.
TEXTURES := $(patsubst textures/%,%,$(wildcard textures/*.png))

# The music: Standard MIDI files of out-of-copyright pieces, all of them
# in the public domain and all of them small (assets/music/MUSIC.md says
# where each came from). They install beside the textures, and a player
# may add their own to /sd/synthminer/music without touching these.
MUSIC := $(patsubst assets/music/%,%,$(wildcard assets/music/*.mid))

MAKEFLAGS += --silent

####

.PHONY: all
all: build

.PHONY: build
build: check
	@echo "=== Building app.so ==="
	mkdir -p $(BUILD)
	cd $(BUILD) && cmake .. && make
	$(MAKE) symcheck
	@echo "=== Build complete: $(BUILD)/app.so ==="

# Every symbol the app calls must be one graceloader exports, or the app
# links fine and then silently refuses to start (tools/symcheck.sh).
# After the link, not in `check`, because it needs app.so.
.PHONY: symcheck
symcheck:
	./tools/symcheck.sh $(BUILD)/app.so

# ---------------------------------------------------------------------
# Host checks: no badge, seconds to run, and `build` depends on them so
# a broken invariant stops the build that broke it
# (claudeplans/synthminer.md, Part H).
#
# The engine's compile-time settings are read straight out of
# CMakeLists.txt rather than written down twice -- a host check that
# tested against different caps than the app builds with would be worse
# than no check at all.
# ---------------------------------------------------------------------
HOSTCC      ?= cc
ENGINE_DEFS := $(shell sed -n 's/^add_compile_definitions(\(SE_[A-Z_]*=[0-9]*\))/-D\1/p' CMakeLists.txt)
HOSTCFLAGS  := -O1 -Wall -Wextra -Werror=implicit-function-declaration \
               -DSM_HOST -Imain -Itools -Isynthengine3D/include \
               -Isynthengine3D/src/internal $(ENGINE_DEFS)

PURE_SRCS       := main/math/xform.c main/math/mesh.c main/voxel/voxel_mesh.c \
                   main/world/blocks.c main/world/chunk.c main/common/rng.c main/common/tags.c \
                   main/world/worldgen.c main/world/farlands.c main/world/chunk_codec.c main/world/region.c main/world/blockent.c \
                   main/world/vfs_compat.c main/world/light.c main/world/worldstore.c main/world/datadir.c main/world/chunkmesh.c main/world/chunk_worker.c \
                   main/game/physics.c main/game/raycast.c main/game/interact.c main/game/furnace.c main/game/daytime.c main/game/replay.c \
                   main/items/items.c main/items/inventory.c main/items/item_entity.c main/items/recipes.c \
                   main/i18n/i18n.c main/i18n/strings_gen.c main/i18n/fold.c \
                   main/audio/midi_seq.c \
                   synthengine3D/src/nbt.c
MESHCHECK_SRCS  := tools/meshcheck.c $(PURE_SRCS)
WORLDCHECK_SRCS := tools/worldcheck.c $(PURE_SRCS)

.PHONY: check
check: hostpurity lang meshcheck worldcheck

# ---------------------------------------------------------------------
# The translations. lang/*.txt is the source and main/i18n/strings_gen.*
# the baked copy that ships, so this is an ordinary generated file with
# known inputs -- make owns it. Edit a lang file, build, and it is
# regenerated; nobody has to remember a second command.
#
# The generator validates while it generates: a key English does not
# have, a %-placeholder that does not match English's, a word mixing two
# alphabets. Whether the FONT can draw every character is worldcheck's
# "languages" section, which compiles the file this rule writes.
#
# The output is committed, so a clone that only compiles needs no Python
# -- and `make langcheck` is the CI form, which asks whether what is
# committed is already up to date instead of bringing it up to date.
# ---------------------------------------------------------------------
LANG_SRCS   := $(wildcard lang/*.txt)
LANG_GEN_C  := main/i18n/strings_gen.c
LANG_GEN_H  := main/i18n/strings_gen.h

$(LANG_GEN_C): $(LANG_SRCS) tools/make_lang.py
	python3 tools/make_lang.py

# Written by the same run; this is here so deleting only the header
# rebuilds it too.
$(LANG_GEN_H): $(LANG_GEN_C)
	@test -f $@ || python3 tools/make_lang.py

# The search box's fold table (main/i18n/fold.h): the same inputs, so
# the same kind of rule. Its generator refuses to emit a table with a
# hole in it, which is what keeps a new language's alphabet from
# quietly becoming unsearchable.
FOLD_GEN_H  := main/i18n/fold_table.h

$(FOLD_GEN_H): $(LANG_SRCS) tools/make_fold.py
	python3 tools/make_fold.py

.PHONY: lang
lang: $(LANG_GEN_C) $(LANG_GEN_H) $(FOLD_GEN_H)

.PHONY: langcheck
langcheck:
	python3 tools/make_lang.py --check
	python3 tools/make_fold.py --check

# The pure set really is pure: no engine, no RTOS, no ESP-IDF.
.PHONY: hostpurity
hostpurity:
	./tools/hostpurity.sh

# The greedy mesher: closed, consistently wound, outward parts; volume
# equals the solid cells and surface area equals the exposed faces.
.PHONY: meshcheck
meshcheck: $(LANG_GEN_C) $(LANG_GEN_H) $(FOLD_GEN_H)
	mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCFLAGS) $(MESHCHECK_SRCS) -lm -o $(BUILD)/host/meshcheck
	$(BUILD)/host/meshcheck

# The world, generation, physics, picking and crafting.
.PHONY: worldcheck
worldcheck: $(LANG_GEN_C) $(LANG_GEN_H) $(FOLD_GEN_H)
	mkdir -p $(BUILD)/host
	$(HOSTCC) $(HOSTCFLAGS) $(WORLDCHECK_SRCS) -lm -o $(BUILD)/host/worldcheck
	$(BUILD)/host/worldcheck

# SynthEngine3D, the 3D engine: not part of the template, added per app as a
# git submodule (CMakeLists.txt builds it when synthengine3D/ is there, and is
# untouched otherwise). Run this once in a new app that wants 3D; afterwards a
# fresh clone needs `git clone --recursive` or `git submodule update --init`.
ENGINE_URL ?= git@github.com:nullislandspace/synthengine3D.git
ENGINE_REF ?= main

.PHONY: engine
engine:
	if test -d synthengine3D; then \
	  echo "synthengine3D/ is already there -- 'git submodule update --remote synthengine3D' updates it"; exit 1; \
	fi
	git submodule add -b $(ENGINE_REF) $(ENGINE_URL) synthengine3D
	git submodule update --init --recursive synthengine3D
	@echo "=== SynthEngine3D added. Commit .gitmodules and synthengine3D, then #include \"synthengine3d.h\" ==="

# Test automation (main/testkit/devtest.h, tools/testrun.py). Opt-in:
# the app has to compile main/testkit/*.c and call devtest_start(). See
# README, "Automated device tests".
#
#   make testrun TEST="perf scene=title secs=20"      the app must be running
#   make cycle   TEST="shots scene=title ms=0,2500"   build, install, run, test
#   make testrefs    TEST="shots ..."                 cycle, then store the shot hashes as references
#   make testcompare TEST="shots ..."                 cycle, then compare against them
#   make recover                                      after a crash or a hang
#
# The app runs the test and returns to the launcher by itself, so the
# whole cycle is hands-free. Shot images stay on the SD card; the hashes
# come back over the console, which is what a regression check compares.
# TESTFLAGS=--fetch downloads the images too (slow).
TEST ?=
TESTFLAGS ?=

.PHONY: testrun cycle testrefs testcompare recover
testrun:
	source "$(IDF_SOURCE)" >/dev/null && \
	python3 -u tools/testrun.py --port "$(PORT)" --badgelink-conn "$(BADGELINK_CONN)" $(TESTFLAGS) -- $(TEST)

cycle: build install run
	$(MAKE) testrun

testrefs:
	$(MAKE) cycle TESTFLAGS="--capture-refs"

testcompare:
	$(MAKE) cycle TESTFLAGS="--compare"

recover:
	source "$(IDF_SOURCE)" >/dev/null && python3 tools/recover.py --port "$(PORT)"

# Badgelink
# --- Talking to the badge ---------------------------------------------
# Thin wrappers over tools/testrun.py's own connection code, which knows
# how this console wants to be opened and retries what the proxy refuses.
#
#   make ping      does the app answer, and which build is it running?
#   make mode      put the badge in BadgeLink mode (probes first)
#   make exitapp   ask a running app to return to the launcher
#
# `ping` is the one to reach for when a cycle has failed and it is not
# clear whether the app is alive, wedged, or never started.
BADGECTL = python3 tools/badgectl.py --port "$(PORT)" --badgelink "$(BADGELINKPORT)"

# These go through tools/testrun.py's serial code, which needs pyserial
# -- so they source the IDF environment like every other device target.
.PHONY: ping
ping:
	source "$(IDF_SOURCE)" >/dev/null && $(BADGECTL) ping

.PHONY: mode
mode:
	source "$(IDF_SOURCE)" >/dev/null && $(BADGECTL) mode

.PHONY: exitapp
exitapp:
	source "$(IDF_SOURCE)" >/dev/null && $(BADGECTL) exitapp

.PHONY: badgelink
badgelink:
	rm -rf badgelink
	#git clone https://github.com/badgeteam/esp32-component-badgelink.git badgelink
	git clone https:///github.com/nullislandspace/esp32-component-badgelink.git badgelink
	cd badgelink/tools; ./install.sh

# Determine badgelink connection argument: --tcp for host:port, --port for serial devices
BADGELINK_CONN := $(if $(findstring :,$(BADGELINKPORT)),--tcp $(BADGELINKPORT),--port $(BADGELINKPORT))

.PHONY: install
install: build mode
	@echo "=== Installing to device ==="
	@echo "Creating directory $(APP_INSTALL_PATH)..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs mkdir $(APP_INSTALL_PATH) || true
	@echo "Uploading metadata.json..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/metadata.json ../../metadata/metadata.json
	@echo "Uploading icon16.png..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/icon16.png ../../metadata/icon16.png
	@echo "Uploading icon32.png..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/icon32.png ../../metadata/icon32.png
	@echo "Uploading icon64.png..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/icon64.png ../../metadata/icon64.png
	@echo "Uploading app.so..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/app.so ../../$(BUILD)/app.so
	@echo "Uploading $(words $(TEXTURES)) textures..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs mkdir $(APP_INSTALL_PATH)/textures >/dev/null 2>&1 || true
	for t in $(TEXTURES); do \
	  cd badgelink/tools && ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/textures/$$t ../../textures/$$t || exit 1; \
	  cd ../..; \
	done
	@echo "Uploading $(words $(MUSIC)) pieces of music..."
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) fs mkdir $(APP_INSTALL_PATH)/music >/dev/null 2>&1 || true
	for m in $(MUSIC); do \
	  cd badgelink/tools && ./badgelink.sh $(BADGELINK_CONN) fs upload $(APP_INSTALL_PATH)/music/$$m ../../assets/music/$$m || exit 1; \
	  cd ../..; \
	done
	@echo "=== Installation complete ==="

# Regenerate the block textures (needs numpy + Pillow). They are committed,
# so this is only needed when a generator changes or a block is added.
.PHONY: textures
textures:
	python3 tools/make_textures.py

GRACELOADER_SLUG ?= at.cavac.graceloader

.PHONY: run
run:
	cd badgelink/tools; ./badgelink.sh $(BADGELINK_CONN) start $(GRACELOADER_SLUG) $(APP_INSTALL_PATH)/app.so

# USB mode switching
#
# The device's USB peripheral is in one of two modes: BadgeLink (USB_DEVICE),
# which is what install / run need, or flash-and-monitor (USB_DEBUG). An
# `install` that fails to reach the device usually means the launcher left it
# in debug mode -- `make mode_badgelink` puts it back.
#
# Ask the firmware (in USB_DEBUG mode) to switch its USB into BadgeLink mode
# by sending the token "BADGELINK\n" on the USB-serial/JTAG peripheral. The
# launcher listens for it (see ../tanmatsu-launcher/main/usb_device.c), so
# this only works against firmware that implements the listener.
#
# PORT accepts either a local device path (e.g. /dev/ttyACM0) or an rfc2217://
# URL pointing at ../tanmatsu-badgefs/rfc2217proxy when the device is
# forwarded over the network.
.PHONY: mode_badgelink
mode_badgelink:
	source "$(IDF_SOURCE)" >/dev/null && \
	python3 -c "import serial, sys; s=serial.serial_for_url('$(PORT)', timeout=1); s.write(b'BADGELINK\n'); s.flush(); sys.stdout.write(s.read(128).decode(errors='replace')); s.close()"

# The other direction: ask the firmware (in BadgeLink mode) to switch its USB
# back to flash/monitor mode, through BadgeLink's own `mode` command. Uses the
# badgelink checkout that `make badgelink` creates, and the same BADGELINK_CONN
# as install / run, so it follows a networked device too.
BADGELINK_SH := badgelink/tools/badgelink.sh

.PHONY: mode_debug
mode_debug:
	if [ ! -x "$(BADGELINK_SH)" ]; then \
	  echo "$(BADGELINK_SH) not found -- run 'make badgelink' first"; \
	  exit 1; \
	fi; \
	echo "Using $(BADGELINK_SH)"; \
	"$(BADGELINK_SH)" $(BADGELINK_CONN) mode debug

APP_REPO_PATH ?= ../tanmatsu-app-repository/$(APP_SLUG_NAME)

.PHONY: apprepo
apprepo: build
	@echo "=== Updating app repository ==="
	mkdir -p $(APP_REPO_PATH)
	cp metadata/metadata.json $(APP_REPO_PATH)/metadata.json
	cp metadata/icon16.png $(APP_REPO_PATH)/icon16.png
	cp metadata/icon32.png $(APP_REPO_PATH)/icon32.png
	cp metadata/icon64.png $(APP_REPO_PATH)/icon64.png
	cp $(BUILD)/app.so $(APP_REPO_PATH)/app.so
	@echo "=== App repository updated at $(APP_REPO_PATH) ==="

# Preparation

.PHONY: prepare
prepare: sdk

.PHONY: sdk
sdk:
	if test -d "$(IDF_PATH)"; then echo -e "ESP-IDF target folder exists!\r\nPlease remove the folder or un-set the environment variable."; exit 1; fi
	if test -d "$(IDF_TOOLS_PATH)"; then echo -e "ESP-IDF tools target folder exists!\r\nPlease remove the folder or un-set the environment variable."; exit 1; fi
	git clone --recursive --branch "$(IDF_VERSION)" https://github.com/espressif/esp-idf.git "$(IDF_PATH)" --depth=1 --shallow-submodules
	cd "$(IDF_PATH)"; git submodule update --init --recursive
	cd "$(IDF_PATH)"; bash install.sh all

.PHONY: reinstallsdk
reinstallsdk:
	cd "$(IDF_PATH)"; bash install.sh all

.PHONY: removesdk
removesdk:
	rm -rf "$(IDF_PATH)"
	rm -rf "$(IDF_TOOLS_PATH)"

.PHONY: refreshsdk
refreshsdk: removesdk sdk

# Verification

.PHONY: verify
verify: build
	@echo "=== Verifying symbols ==="
	@MISSING=$$(comm -23 \
	  <(nm -D --undefined-only $(BUILD)/app.so | awk '{print $$2}' | sort -u) \
	  <(nm -D --defined-only fakelib/liball.so | awk '{print $$3}' | sort -u)); \
	if [ -n "$$MISSING" ]; then \
	  echo "ERROR: app.so requires symbols not in fakelib:"; \
	  echo "$$MISSING"; \
	  exit 1; \
	else \
	  echo "All symbols satisfied."; \
	fi

# Cleaning

.PHONY: clean
clean:
	rm -rf $(BUILD)

.PHONY: fullclean
fullclean: clean

# Formatting

.PHONY: format
format:
	find main/ -iname '*.h' -o -iname '*.c' -o -iname '*.cpp' | xargs clang-format -i
