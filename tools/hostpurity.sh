#!/usr/bin/env bash
# =====================================================================
#  CraftMiner  --  the host-purity rule
# ---------------------------------------------------------------------
#  A module in the "pure" set must compile with a plain host compiler,
#  so that tools/worldcheck.c can test the world, the mesher, physics,
#  picking and crafting on a PC in seconds instead of on the badge in
#  minutes (claudeplans/craftminer.md, Part H).
#
#  The rule: no engine header, no RTOS header, no ESP-IDF header. The
#  one thing these modules need that differs between host and badge is
#  allocation, and that is main/common/psram.h.
#
#  This runs in `make check`, which `make build` depends on, so the seam
#  cannot rot silently: the first #include "esp_log.h" added to a pure
#  module fails the build that adds it.
# =====================================================================
set -u

PURE=(
    main/math/xform.c       main/math/xform.h
    main/math/mesh.c        main/math/mesh.h
    main/voxel/voxel_mesh.c main/voxel/voxel_mesh.h
    main/world/blocks.c     main/world/blocks.h
    main/world/chunk.c      main/world/chunk.h
    main/world/light.c      main/world/light.h
    main/common/rng.c       main/common/rng.h
    main/world/worldgen.c   main/world/worldgen.h
    main/world/chunk_codec.c main/world/chunk_codec.h
    main/world/region.c     main/world/region.h
    main/common/tags.c      main/common/tags.h
    main/game/physics.c     main/game/physics.h
    main/game/raycast.c     main/game/raycast.h
    main/game/interact.c    main/game/interact.h
    main/game/daytime.c     main/game/daytime.h
    main/game/replay.c      main/game/replay.h
    main/items/items.c      main/items/items.h
    main/items/inventory.c  main/items/inventory.h
    main/items/item_entity.c main/items/item_entity.h
)

# Headers a pure module must not reach for. psram.h is the sanctioned
# way to get at PSRAM, and it is the only file allowed to include
# esp_heap_caps.h.
BANNED='esp_log\.h|esp_heap_caps\.h|esp_timer\.h|esp_system\.h|esp_err\.h|freertos/|synthengine3d\.h|se_[a-z_]*\.h|pax_gfx\.h|bsp/|graceloader\.h'

fail=0
for f in "${PURE[@]}"; do
    if [ ! -e "$f" ]; then
        echo "hostpurity: FAIL: $f is listed as pure but does not exist"
        fail=1
        continue
    fi
    hits=$(grep -nE "^[[:space:]]*#[[:space:]]*include.*($BANNED)" "$f" || true)
    if [ -n "$hits" ]; then
        echo "hostpurity: FAIL: $f is in the pure set but includes an engine/RTOS header:"
        echo "$hits" | sed 's/^/    /'
        fail=1
    fi
done

if [ "$fail" -ne 0 ]; then
    echo "hostpurity: the pure set must build with a plain host compiler (Part H)."
    exit 1
fi
echo "hostpurity: ${#PURE[@]} files, all clean"
