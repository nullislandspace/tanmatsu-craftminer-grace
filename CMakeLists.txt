cmake_minimum_required(VERSION 3.16)

# Use the RISC-V cross-compiler toolchain
if(NOT CMAKE_TOOLCHAIN_FILE)
    set(CMAKE_TOOLCHAIN_FILE "${CMAKE_CURRENT_SOURCE_DIR}/toolchain-app.cmake")
endif()

project(app C)

# App source files — add your .c files here
set(APP_SOURCES
    main/main.c
    main/crt0.c

    # Maths and meshes, lifted from the showreel (see each file's header).
    main/math/xform.c
    main/math/mesh.c
    main/math/mesh_render.c
    main/math/camera.c

    # The voxel layer. The greedy mesher is pure data and is also built
    # by the host checks (`make check`); world/blocks.c is the registry
    # it and everything else read instead of switching on block ids.
    main/voxel/voxel_mesh.c
    main/voxel/voxel_sky.c
    main/voxel/starfield.c
    main/world/blocks.c
    main/world/chunk.c
    main/world/light.c
    main/world/worldgen.c
    main/world/farlands.c
    main/world/chunk_codec.c
    main/world/region.c
    main/world/blockent.c
    main/world/vfs_compat.c
    main/world/worldstore.c
    main/world/datadir.c
    main/world/chunkmesh.c
    main/world/chunk_worker.c
    main/world/chunk_render.c
    main/common/rng.c
    main/common/tags.c

    # A camera to fly by hand, and the player who walks.
    main/game/flycam.c
    main/game/membench.c
    main/game/physics.c
    main/game/raycast.c
    main/game/interact.c
    main/game/furnace.c
    main/game/input.c
    main/game/player.c
    main/game/tick.c
    main/game/daytime.c
    main/game/replay.c
    main/game/hud.c

    # Items, and the inventory that holds them.
    main/items/items.c
    main/items/inventory.c
    main/items/item_entity.c
    main/items/recipes.c

    # Fred, the player's figure (from the showreel's miner).
    main/fred/fred.c
    main/fred/fred_mesh.c

    # Screens.
    main/ui/title.c
    main/ui/menu.c
    main/ui/craft_ui.c
    main/ui/furnace_ui.c
    main/ui/chest_ui.c
    main/ui/bench_ui.c
    main/ui/keybind_ui.c
    main/ui/icons.c
    main/ui/settings.c

    # The UI's words, in thirty-two languages (lang/*.txt -> strings_gen.c).
    main/i18n/i18n.c
    main/i18n/fold.c
    main/i18n/strings_gen.c

    # Sound: the mixer's lifetime, the effects table, and the music --
    # a MIDI sequencer and a small synth, because a piece of Satie is
    # eight kilobytes as a MIDI file and four megabytes as an MP3.
    main/audio/audio.c
    main/audio/sfx.c
    main/audio/music.c
    main/audio/midi_seq.c
    main/audio/midi_synth.c

    # Textures by name, cached.
    main/common/texcache.c
)

# Automated device tests (main/testkit/, README "Automated device tests").
# Opt-in: uncomment to build them in, and set where shots go. An app
# without SynthEngine3D adds TESTKIT_NO_ENGINE to the definitions.
list(APPEND APP_SOURCES
    main/testkit/debugcon.c
    main/testkit/devtest.c
    main/testkit/profile.c
    main/testkit/report.c
    main/testkit/screenshot.c
    main/testkit/showtime.c
)
add_compile_definitions(SCREENSHOT_DIR="/sd/craftminer/test")

# Build identity for the debug console's READY records: the git commit
# (marked -dirty when there are uncommitted changes) and the build time.
# `make build` re-runs cmake, so this is fresh on every build. Without it
# the app reports "unknown" and tools/testrun.py refuses to believe any
# result, on the grounds that it cannot tell which build produced it.
execute_process(
    COMMAND git describe --always --dirty --abbrev=10
    WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}
    OUTPUT_VARIABLE APP_GIT_HASH
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
)
if(NOT APP_GIT_HASH)
    set(APP_GIT_HASH "unknown")
endif()
string(TIMESTAMP APP_BUILD_TIME "%Y-%m-%dT%H:%M:%SZ" UTC)
configure_file(main/app_version.h.in ${CMAKE_CURRENT_BINARY_DIR}/generated/app_version.h @ONLY)

# Include directories — add paths to your headers here
set(APP_INCLUDES
    main
    include
    include/esp_additions/include
    ${CMAKE_CURRENT_BINARY_DIR}/generated
)

# Compile app sources as a relocatable object library
add_library(app_obj OBJECT ${APP_SOURCES})
target_include_directories(app_obj PRIVATE ${APP_INCLUDES})
target_compile_options(app_obj PRIVATE
    -Wall -Wextra -Wno-unused-parameter
    # -O2, not the template's -Os. The app is loaded into PSRAM, where
    # code size is the one resource that is not scarce, and the inner
    # loops here are real: the greedy mesher, the per-triangle submit,
    # the chunk streamer. Measured on the engine, the same switch was
    # worth 12% of the rasterise (claudeplans/craftminer.md, F-39).
    -O2 -fPIC
    -ffunction-sections -fdata-sections -fno-common
    -D__thread=
    -DESP_PLATFORM
)

# ESP-IDF normally force-includes FreeRTOS headers for all components
target_compile_options(app_obj PRIVATE
    "SHELL:-include freertos/FreeRTOS.h"
    "SHELL:-include freertos/semphr.h"
    "SHELL:-include freertos/task.h"
    "SHELL:-include freertos/queue.h"
    "SHELL:-include freertos/event_groups.h"
)

# SynthEngine3D, the 3D engine (https://github.com/nullislandspace/synthengine3D),
# if this app has it. It is not part of the template: an app that wants 3D adds
# it as a git submodule at synthengine3D/ (`make engine`, see README), and this
# block then builds it. Without it every variable below stays empty and the
# link line is exactly the one a plain app has always had, so apps that do not
# use the engine are unaffected by merges from the template.
#
# In this plain-CMake build the engine is a relocatable OBJECT library, so
# add_subdirectory MUST come before the link script's file(GENERATE) for
# $<TARGET_OBJECTS:synthengine3d> to resolve. Linking it to app_obj propagates
# the engine's PUBLIC include directory (synthengine3D/include), which is what
# lets app sources #include "synthengine3d.h"; its object files are folded into
# app.so by the link script below, since the custom command, not CMake, links.
set(ENGINE_OBJS "")
set(ENGINE_DEPS "")
# Engine settings. These are compile definitions that must reach the
# `synthengine3d` target, so they go BEFORE add_subdirectory() -- a cap the
# app and the engine disagree on is a silent bug. `make check` reads them
# back out of this file with sed, so the host budget check can never test
# against different values than the app builds with.
#
#   SE_SCENE_TEXTURED_TRI_CAP  the showreel's block world peaked at 1785
#       textured triangles from a showreel camera (its F-36); a player
#       facing a forest will pass the 1024 default, and an overflow drops
#       geometry SILENTLY. 2048 costs about 70 KB of PSRAM and nothing
#       per frame. Flat triangles stay on the 4096 default. (D-09)
#   SE_BINDINGS_MAX  the default 16 is below the ~20 actions CraftMiner
#       declares, and se_bindings_init() CLAMPS rather than complaining,
#       so the last actions would simply never fire. (F-11)
# The near clip plane, down from the engine's default 0.5.
#
# THE PLAYER'S EYE IS 0.3 FROM A WALL THEY ARE TOUCHING (half of a
# 0.6-wide body), and 0.18 below a ceiling they are standing under. At
# 0.5 the engine clipped both away and you saw straight through the
# world -- pressing your face to a tree made it vanish.
#
# The cost is depth precision and range, both proportional to the near
# plane (se_config.h): the far limit drops from 32000 blocks to 6400,
# which is still ninety times the furthest this game draws, and the
# depth step at 40 blocks (the near view's draw distance) is a quarter
# of a block. A voxel world has almost no coplanar surfaces to
# z-fight, so that is a trade worth making to stop seeing through
# walls.
add_compile_definitions(RENDER_NEAR_CLIP_Z=0.1f)
# The geometry lists (F-63). Lighting splits merged faces and the medium
# view draws more of them, and past a cap the engine DROPS triangles --
# nearby terrain vanished, and you saw "through the ground" at a step.
#   flat 6144      240 KB, in PSRAM since the depth plane took its
#                  internal SRAM (below); peak seen 4897 (far view)
#   lines 256      CraftMiner draws a dozen (the block outline); the
#                  default 4096 was 112 KB of internal SRAM for nothing
#   textured 4096  in PSRAM (it never fitted internal), 272 KB
add_compile_definitions(SE_SCENE_TRI_CAP=6144)
add_compile_definitions(SE_SCENE_LINE_CAP=256)
add_compile_definitions(SE_SCENE_TEXTURED_TRI_CAP=4096)
# The quarter-resolution depth plane in internal SRAM, 188 KB (F-67):
# every covered pixel reads it, so it is the one buffer here touched at
# random; the lists it displaces are written and read in order.
add_compile_definitions(SE_SCENE_DEPTH16_INTERNAL=1)
add_compile_definitions(SE_BINDINGS_MAX=24)

if(EXISTS ${CMAKE_CURRENT_SOURCE_DIR}/synthengine3D/CMakeLists.txt)
    message(STATUS "SynthEngine3D: building synthengine3D/")
    add_subdirectory(synthengine3D)
    target_link_libraries(app_obj PRIVATE synthengine3d)
    set(ENGINE_OBJS "$<JOIN:$<TARGET_OBJECTS:synthengine3d>, >")
    list(APPEND ENGINE_DEPS synthengine3d $<TARGET_OBJECTS:synthengine3d>)
endif()

# Collect object files into a list file, then link via a response file
# This avoids issues with generator expressions in custom commands
set(LINK_SCRIPT ${CMAKE_BINARY_DIR}/link_app.sh)
file(GENERATE OUTPUT ${LINK_SCRIPT} CONTENT
"#!/bin/sh
${CMAKE_C_COMPILER} \
    -march=rv32imafc_zicsr_zifencei_zaamo_zalrsc_xesploop_xespv2p1 -mabi=ilp32f \
    -shared -nostdlib \
    -Wl,--gc-sections \
    -Wl,-Bsymbolic \
    -Wl,--version-script=${CMAKE_CURRENT_SOURCE_DIR}/app_version.script \
    -Wl,-T,${CMAKE_CURRENT_SOURCE_DIR}/app.ld \
    -Wl,-Map=${CMAKE_BINARY_DIR}/app.map \
    -L${CMAKE_CURRENT_SOURCE_DIR}/fakelib \
    $<JOIN:$<TARGET_OBJECTS:app_obj>, > ${ENGINE_OBJS} \
    -lall \
    -o ${CMAKE_BINARY_DIR}/app.so
")

add_custom_command(OUTPUT ${CMAKE_BINARY_DIR}/app.so
    COMMAND sh ${LINK_SCRIPT}
    DEPENDS app_obj
        $<TARGET_OBJECTS:app_obj>
        ${ENGINE_DEPS}
        ${CMAKE_CURRENT_SOURCE_DIR}/app.ld
        ${CMAKE_CURRENT_SOURCE_DIR}/app_version.script
        ${CMAKE_CURRENT_SOURCE_DIR}/fakelib/liball.so
    COMMENT "Linking app.so"
)

add_custom_target(app ALL DEPENDS ${CMAKE_BINARY_DIR}/app.so)

# Print size after build
add_custom_command(TARGET app POST_BUILD
    COMMAND ${CMAKE_SIZE} ${CMAKE_BINARY_DIR}/app.so
    COMMENT "app.so size:"
)
