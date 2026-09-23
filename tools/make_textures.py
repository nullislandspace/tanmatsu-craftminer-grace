#!/usr/bin/env python3
"""Generate CraftMiner's block textures into textures/ (one
subdirectory per reel segment, as the code in main/<segment>/).

Procedural and seeded, so the PNGs are reproducible from this script
instead of being opaque binaries: change a number here, re-run, commit
both. Every texture tiles seamlessly -- all noise is built periodic
(FFT smoothing wraps around) and every feature is drawn modulo the
texture size -- because the ship repeats them across large faces.

Plates are 64x64: the engine needs power-of-two edges (it wraps with a
mask), and at 2 bytes a texel in RGB565 a plate costs 8 KB of internal
SRAM. The flame is 64x8 (1 KB); the two planet maps are 128x64
(equirectangular, 16 KB).

    python3 tools/make_textures.py            # write textures/<segment>/*.png
    python3 tools/make_textures.py --preview  # also write a 4x contact sheet
"""

# Lifted from tanmatsu-showreel-grace (tools/make_textures.py); trimmed to CraftMiner's
# own blocks. The showreel remains the origin to diff against.
import sys
from pathlib import Path

import numpy as np
from PIL import Image

N = 64
OUT = Path(__file__).resolve().parent.parent / "textures"
rng = np.random.default_rng(0x5E3D)


def periodic_noise(sx, sy, gen=None):
    """Tileable noise: white noise low-passed with a Gaussian in the FFT
    domain (convolution there is circular, so the result wraps). sx/sy
    are the blur radii in texels; unequal radii give streaks. `gen` is
    the random generator (default: the shared one of the hull plates)."""
    white = (gen or rng).standard_normal((N, N))
    fy = np.fft.fftfreq(N)[:, None]
    fx = np.fft.fftfreq(N)[None, :]
    g = np.exp(-2 * (np.pi ** 2) * ((fx * sx) ** 2 + (fy * sy) ** 2))
    out = np.real(np.fft.ifft2(np.fft.fft2(white) * g))
    return out / (np.abs(out).max() + 1e-9)          # roughly -1..1


def to_rgb(lum, tint):
    """Luminance offsets around a base colour -> clipped uint8 RGB."""
    base = np.array(tint, dtype=float)[None, None, :]
    return np.clip(base + lum[:, :, None], 0, 255).astype(np.uint8)


def seam(lum, dark=-48, light=22):
    """A panel seam along row 0 / column 0, lit from the top-left: a dark
    groove with a bright lip below / right of it. Repeating the texture
    turns that into a plate grid."""
    lum[0, :] += dark
    lum[:, 0] += dark
    lum[1, 1:] += light
    lum[1:, 1] += light


def rivet(lum, x, y):
    """A 3x3 domed rivet head: bright top-left, shadow bottom-right."""
    for dy in range(-1, 2):
        for dx in range(-1, 2):
            lum[(y + dy) % N, (x + dx) % N] += 26 - 16 * (dx + dy)
    lum[(y + 2) % N, (x + 2) % N] -= 34


def riveted():
    """Fuselage: mid steel, soft mottling, one plate per tile with a
    rivet line inset along its two seams."""
    lum = 9 * periodic_noise(6, 6) + 4 * periodic_noise(1.2, 1.2)
    seam(lum)
    for k in range(4, N, 8):
        rivet(lum, k, 4)
        rivet(lum, 4, k)
    return to_rgb(lum, (148, 151, 156))


def brushed():
    """Wings: bright brushed steel -- long horizontal grain -- with a
    single seam, so the wing reads as a few large sheets."""
    lum = 15 * periodic_noise(14, 0.45) + 5 * periodic_noise(3, 0.8)
    seam(lum, dark=-40, light=18)
    return to_rgb(lum, (176, 179, 184))


def gunmetal():
    """Pods: dark blue-grey, vertical cooling grooves every 8 texels and
    a few bright scratches."""
    lum = 7 * periodic_noise(5, 5)
    for x in range(0, N, 8):
        lum[:, x] -= 30
        lum[:, (x + 1) % N] += 14
    for _ in range(6):
        x0, y0 = rng.integers(0, N, 2)
        length = rng.integers(8, 22)
        dx = rng.choice([-1, 1])
        for i in range(length):
            lum[(y0 + i) % N, (x0 + dx * i) % N] += 30
    return to_rgb(lum, (92, 98, 110))


def tread():
    """Undersides: diamond tread plate -- raised lozenges on a 16-texel
    grid, alternating 45-degree orientation checkerboard-style, each lit
    from the top-left."""
    lum = 6 * periodic_noise(4, 4)
    yy, xx = np.mgrid[0:N, 0:N]
    cell = 16
    for cy in range(0, N, cell):
        for cx in range(0, N, cell):
            flip = ((cx // cell) + (cy // cell)) % 2
            mx, my = cx + cell // 2, cy + cell // 2
            # Distance to the cell centre, wrapped so edge lozenges tile.
            ddx = (xx - mx + N // 2) % N - N // 2
            ddy = (yy - my + N // 2) % N - N // 2
            a = (ddx + ddy) if not flip else (ddx - ddy)   # along the lozenge
            b = (ddx - ddy) if not flip else (ddx + ddy)   # across it
            body = (np.abs(a) <= 7) & (np.abs(b) <= 1.5)
            lum[body] += 22
            # Lit edge on the upper-left side, shadow on the lower-right.
            edge = (np.abs(a) <= 7) & (np.abs(b) > 1.5) & (np.abs(b) <= 2.6)
            upper = (ddy + ddx) < 0 if flip else (ddy - ddx) < 0
            lum[edge & upper] += 12
            lum[edge & ~upper] -= 26
    return to_rgb(lum, (122, 124, 129))


def flame():
    """Engine flame, 64x8: u (x) runs along the flame from the nozzle
    (x = 0) to the tip, v (y) around it. A white-hot core fading through
    blue to a deep blue tip, with faint lengthwise streaks that fade in
    after the core so the white stays clean. Drawn emissive, so these
    are the exact on-screen colours. Not tiled along u: the flame maps it
    once, stopping short of the right edge so the tip never wraps back
    to white."""
    w, h = 64, 8
    stops = [(0.00, (235, 250, 255)), (0.15, (150, 212, 255)), (0.40, (60, 132, 255)),
             (0.75, (26, 62, 205)), (1.00, (10, 26, 112))]
    xs = np.linspace(0.0, 1.0, w)
    ramp = np.zeros((w, 3))
    for c in range(3):
        ramp[:, c] = np.interp(xs, [p for p, _ in stops], [col[c] for _, col in stops])
    streak = np.array([1.00, 0.90, 1.06, 0.94, 1.00, 0.88, 1.05, 0.95])[:, None, None]
    fade = np.clip((xs - 0.12) / 0.3, 0.0, 1.0)[None, :, None]   # no streaks in the core
    img = ramp[None, :, :] * (1.0 + (streak - 1.0) * fade)
    return np.clip(img, 0, 255).astype(np.uint8)


# --- Station and marauder textures -----------------------------------
#
# Their own generator, so adding them left the hull plates above (which
# share `rng`, in order) byte-identical.
rng2 = np.random.default_rng(0x57A7)


def panel_grid(lum, step, dark=-34, light=14):
    """Panel seams every `step` texels (tileable when step divides N)."""
    for k in range(0, N, step):
        lum[k, :] += dark
        lum[:, k] += dark
        lum[(k + 1) % N, :] += light
        lum[:, (k + 1) % N] += light


def station_hull():
    """Station hub and faces: the 2001 station's off-white, large panels
    (two per tile each way), soft mottling and a few small dark vents."""
    lum = 5 * periodic_noise(7, 7, rng2) + 2 * periodic_noise(1.5, 1.5, rng2)
    panel_grid(lum, 32)
    for _ in range(5):
        x0, y0 = rng2.integers(2, N - 8, 2)
        w, h = rng2.integers(3, 7), rng2.integers(2, 4)
        lum[y0:y0 + h, x0:x0 + w] -= 70
        lum[y0 + h, x0:x0 + w] += 20      # lit lower lip
    return to_rgb(lum, (200, 202, 204))


def station_ring():
    """Ring walls: the hull panelling with a band of lit windows across
    the middle -- read as habitation. (Lit only by the scene light, not
    emissive: on the night side the windows go dark with the wall.)"""
    lum = 5 * periodic_noise(7, 7, rng2)
    panel_grid(lum, 32)
    img = to_rgb(lum, (198, 200, 202)).astype(int)
    band = slice(26, 38)
    img[band, :, :] = (img[band, :, :] * 0.35).astype(int)      # dark window band
    for x in range(2, N, 8):                                     # 8 windows per tile
        on = rng2.random() < 0.8
        col = (255, 226, 150) if on else (60, 66, 80)
        img[29:35, x:x + 4, :] = col
    return np.clip(img, 0, 255).astype(np.uint8)


def marauder_wear():
    """Shared by both marauder liveries, so the two ships are visibly the
    same type: panel seams, grime (a luminance field) and a mask of where
    the paint is scratched or chipped down to bare metal."""
    grime = 16 * periodic_noise(5, 5, rng2) + 6 * periodic_noise(1.2, 1.2, rng2)
    panel_grid(grime, 16, dark=-40, light=10)
    bare = np.zeros((N, N), bool)
    for _ in range(9):                                   # scratches
        x0, y0 = rng2.integers(0, N, 2)
        length = rng2.integers(6, 20)
        dx = rng2.choice([-1, 1])
        for i in range(length):
            bare[(y0 + i // 2) % N, (x0 + dx * i) % N] = True
    chips = periodic_noise(2.2, 2.2, rng2) > 0.55        # chipped patches
    return grime, bare | chips


_WEAR = None


def marauder(paint):
    global _WEAR
    if _WEAR is None:
        _WEAR = marauder_wear()
    grime, bare = _WEAR
    img = to_rgb(grime, paint).astype(int)
    metal = to_rgb(grime * 0.6, (128, 130, 134)).astype(int)
    img[bare] = metal[bare]
    return np.clip(img, 0, 255).astype(np.uint8)


def marauder_green():
    """Marauder #1: old, dirty green paint."""
    return marauder((70, 94, 56))


def marauder_yellow():
    """Marauder #2: darkened, sooty yellow."""
    return marauder((150, 124, 38))


def flame_red():
    """Marauder engine flame, 64x8: the counterpart of flame(), running
    white-hot -> orange -> deep red."""
    w, h = 64, 8
    stops = [(0.00, (255, 246, 222)), (0.15, (255, 196, 96)), (0.40, (255, 112, 32)),
             (0.75, (196, 38, 16)), (1.00, (92, 12, 8))]
    xs = np.linspace(0.0, 1.0, w)
    ramp = np.zeros((w, 3))
    for c in range(3):
        ramp[:, c] = np.interp(xs, [p for p, _ in stops], [col[c] for _, col in stops])
    streak = np.array([1.00, 0.90, 1.06, 0.94, 1.00, 0.88, 1.05, 0.95])[:, None, None]
    fade = np.clip((xs - 0.12) / 0.3, 0.0, 1.0)[None, :, None]
    img = ramp[None, :, :] * (1.0 + (streak - 1.0) * fade)
    return np.clip(img, 0, 255).astype(np.uint8)


# --- Planet base, asteroid and planet textures (full reel, step 9.1) ---
#
# Again their own generator, so everything above stays byte-identical.
rng3 = np.random.default_rng(0xB0D5)


def pnoise(h, w, sx, sy, gen=rng3):
    """periodic_noise() for an h x w texture (the planet maps are 128x64)."""
    white = gen.standard_normal((h, w))
    fy = np.fft.fftfreq(h)[:, None]
    fx = np.fft.fftfreq(w)[None, :]
    g = np.exp(-2 * (np.pi ** 2) * ((fx * sx) ** 2 + (fy * sy) ** 2))
    out = np.real(np.fft.ifft2(np.fft.fft2(white) * g))
    return out / (np.abs(out).max() + 1e-9)


def ground():
    """The apron round the landing pad: packed ochre dust, darker gravel
    speckle, a few paler patches. Its average colour is what the PPA's flat
    ground has to match where the apron ends."""
    lum = 12 * pnoise(N, N, 6, 6) + 7 * pnoise(N, N, 1.4, 1.4) + 5 * pnoise(N, N, 0.6, 0.6)
    speck = rng3.random((N, N))
    lum[speck < 0.06] -= 26                       # gravel
    lum[speck > 0.97] += 18                       # pale grit
    return to_rgb(lum, (112, 92, 64))


def pad():
    """Landing pad: poured concrete slabs (two per tile each way) with
    tar-filled joints, scorch mottling from engine blasts and oil stains."""
    lum = 6 * pnoise(N, N, 5, 5) + 3 * pnoise(N, N, 1.0, 1.0)
    for k in (0, 32):
        lum[k, :] -= 46
        lum[:, k] -= 46
        lum[(k + 1) % N, :] += 10
        lum[:, (k + 1) % N] += 10
    lum += np.minimum(0, 30 * pnoise(N, N, 9, 9)) * 1.2   # soot, only darkens
    for _ in range(4):                                     # oil stains
        cx, cy = rng3.integers(0, N, 2)
        r = rng3.integers(2, 5)
        yy, xx = np.mgrid[0:N, 0:N]
        d = ((xx - cx + N // 2) % N - N // 2) ** 2 + ((yy - cy + N // 2) % N - N // 2) ** 2
        lum[d <= r * r] -= 30
    return to_rgb(lum, (150, 148, 142))


def industrial_wall():
    """Factory siding: vertical corrugated steel (a rib every 4 texels, lit
    from the left), one horizontal panel seam per tile, and rust running
    down from the seam."""
    xs = np.arange(N)
    rib = 16 * np.cos(2 * np.pi * xs / 4.0)[None, :].repeat(N, axis=0)
    lum = rib + 5 * pnoise(N, N, 4, 4)
    lum[0, :] -= 40
    lum[1, :] += 14
    img = to_rgb(lum, (118, 124, 128)).astype(int)
    rust = np.clip(pnoise(N, N, 1.2, 9) * 1.4, 0, 1)       # streaks: stretched vertically
    fall = np.linspace(1.0, 0.2, N)[:, None]               # strongest just below the seam
    w = (rust * fall)[:, :, None]
    img = img * (1 - w) + np.array([124, 70, 38])[None, None, :] * w
    return np.clip(img, 0, 255).astype(np.uint8)


def rock():
    """Asteroid: grey-brown rock at several scales, pocked with small
    craters (dark bowl, bright rim on the lit upper-left side)."""
    lum = 18 * pnoise(N, N, 8, 8) + 10 * pnoise(N, N, 2.5, 2.5) + 5 * pnoise(N, N, 0.7, 0.7)
    yy, xx = np.mgrid[0:N, 0:N]
    for _ in range(9):
        cx, cy = rng3.integers(0, N, 2)
        r = rng3.uniform(2.0, 6.0)
        dx = (xx - cx + N // 2) % N - N // 2
        dy = (yy - cy + N // 2) % N - N // 2
        d = np.sqrt(dx * dx + dy * dy)
        lum[d < r] -= 22
        rim = (d >= r) & (d < r + 1.5)
        lum[rim & (dx + dy < 0)] += 20
        lum[rim & (dx + dy >= 0)] -= 10
    return to_rgb(lum, (112, 104, 96))


def planet_terran():
    """The industrial planet from orbit, 128x64 equirectangular (u wraps
    round the equator): ochre continents, dark slate seas, grey ice at the
    poles and thin cloud."""
    h, w = 64, 128
    height = pnoise(h, w, 7, 7) + 0.35 * pnoise(h, w, 2, 2)
    land = height > 0.05
    img = np.zeros((h, w, 3))
    img[:] = (44, 60, 72)                                   # sea
    shade = (height - 0.05)[:, :, None] * 120
    img[land] = (np.array([150, 118, 72])[None, :] + shade[land]).clip(0, 255)
    lat = np.abs(np.linspace(-1, 1, h))[:, None]
    ice = lat + 0.08 * pnoise(h, w, 3, 3) > 0.82
    img[ice] = (196, 200, 206)
    cloud = np.clip((pnoise(h, w, 4, 1.5) - 0.25) * 2.2, 0, 1)[:, :, None]
    img = img * (1 - 0.7 * cloud) + 225 * 0.7 * cloud
    return np.clip(img, 0, 255).astype(np.uint8)


def planet_gas():
    """The gas giant of the second system, 128x64 equirectangular:
    latitude bands in cream, tan and rust, their edges torn by turbulence,
    and one oval storm."""
    h, w = 64, 128
    y = np.linspace(-1, 1, h)[:, None].repeat(w, axis=1)
    warp = 0.09 * pnoise(h, w, 10, 2.5) + 0.03 * pnoise(h, w, 2, 1)
    yw = y + warp
    # Broad belts of uneven width: two latitude frequencies beating.
    b = 0.65 * np.sin(yw * np.pi * 3.1) + 0.35 * np.sin(yw * np.pi * 7.7 + 1.3)
    stops = np.array([[92, 50, 34], [176, 118, 72], [222, 196, 150], [196, 150, 100]], float)
    t = (b + 1) / 2 * (len(stops) - 1)
    i = np.clip(t.astype(int), 0, len(stops) - 2)
    f = (t - i)[:, :, None]
    img = stops[i] * (1 - f) + stops[i + 1] * f
    yy, xx = np.mgrid[0:h, 0:w]
    storm = ((xx - 88) / 9.0) ** 2 + ((yy - 40) / 4.0) ** 2
    img[storm < 1] = img[storm < 1] * 0.4 + np.array([200, 96, 60]) * 0.6
    img[(storm >= 1) & (storm < 1.6)] *= 1.12
    return np.clip(img, 0, 255).astype(np.uint8)


# --- CraftMiner block textures (claudeplans/craftminer.md, step 1.1) ---
#
# 16x16, drawn texel by texel (the engine samples nearest-texel, so each
# texel shows as a crisp square). Every texture has its own generator,
# seeded from its own tag, so adding or reordering one leaves the others
# byte-identical. All tile: features wrap modulo 16.
B = 16


def cm_gen(tag):
    return np.random.default_rng([0xC4AF7, tag])


def cm_speckle(gen, tint, spread, h=B, w=B):
    """A base colour with independent per-texel brightness steps -- the
    pixel-art grain of every block."""
    lum = gen.integers(-spread, spread + 1, (h, w)).astype(float)
    return lum, tint


def cm_rgb(lum, tint):
    return to_rgb(lum, tint)


def cm_dirt_lum(gen):
    lum = gen.integers(-14, 15, (B, B)).astype(float)
    for _ in range(10):                               # darker clods, lighter grit
        x, y = gen.integers(0, B, 2)
        lum[y, x] -= 22
    for _ in range(6):
        x, y = gen.integers(0, B, 2)
        lum[y, x] += 18
    return lum


DIRT = (122, 86, 58)
GRASS = (92, 150, 52)


def cm_dirt():
    return cm_rgb(cm_dirt_lum(cm_gen(1)), DIRT)


def cm_grass_top():
    gen = cm_gen(2)
    lum = gen.integers(-16, 17, (B, B)).astype(float)
    for _ in range(14):                               # darker blades
        x, y = gen.integers(0, B, 2)
        lum[y, x] -= 20
    return cm_rgb(lum, GRASS)


def cm_grass_side():
    """Dirt with the grass hanging over the top edge, 2-5 texels deep per
    column. v = 0 is the top of the block."""
    gen = cm_gen(3)
    img = cm_rgb(cm_dirt_lum(gen), DIRT).astype(int)
    grass = cm_rgb(gen.integers(-16, 17, (B, B)).astype(float), GRASS).astype(int)
    depth = 3 + gen.integers(-1, 2, B)
    depth[gen.integers(0, B, 3)] += 2                 # a few longer drips
    for x in range(B):
        img[: depth[x], x] = grass[: depth[x], x]
    return img.astype(np.uint8)


def cm_stone():
    gen = cm_gen(4)
    lum = 10 * pnoise(B, B, 1.2, 1.2, gen) + gen.integers(-8, 9, (B, B))
    for _ in range(3):                                # short dark streaks
        x, y = gen.integers(0, B, 2)
        n = int(gen.integers(2, 5))
        for k in range(n):
            lum[y % B, (x + k) % B] -= 20
            y += int(gen.integers(0, 2))
    return cm_rgb(lum, (122, 122, 124))


def cm_cobble():
    """Rounded stones in mortar: each texel belongs to its nearest seed
    (distance wraps, so the pattern tiles); texels near a cell edge are
    mortar."""
    gen = cm_gen(5)
    # One stone per cell of a jittered 3x3 grid, so they spread evenly.
    grid = (np.mgrid[0:3, 0:3].reshape(2, -1).T + 0.5) * (B / 3)
    seeds = grid + gen.uniform(-1.3, 1.3, grid.shape)
    shade = gen.integers(-9, 10, len(seeds))
    ys, xs = np.mgrid[0:B, 0:B] + 0.5
    d = []
    for sx, sy in seeds:
        dx = np.abs(xs - sx)
        dy = np.abs(ys - sy)
        dx = np.minimum(dx, B - dx)
        dy = np.minimum(dy, B - dy)
        d.append(np.sqrt(dx * dx + dy * dy))
    d = np.array(d)
    order = np.sort(d, axis=0)
    cell = np.argmin(d, axis=0)
    # Rounded: brightest at the stone's middle, darker towards its edge.
    lum = shade[cell] + 22 - 2.2 * order[0] + gen.integers(-5, 6, (B, B))
    lum[(order[1] - order[0]) < 0.7] = -40             # mortar
    return cm_rgb(lum, (118, 118, 118))


def cm_sand():
    gen = cm_gen(6)
    lum = gen.integers(-10, 11, (B, B)).astype(float)
    return cm_rgb(lum, (214, 200, 150))


def cm_water():
    """The surface of water, and the only face a liquid ever draws
    (blocks.h, K_LIQUID): deep blue with lighter wave crests running
    across u, so a scrolled u reads as flowing.

    CUT-OUT, on a checkerboard. The engine has no blending -- alpha is
    one bit -- so the way to make water look like water rather than like
    a blue floor is to punch every other texel out and let what is
    behind show through it. From above that is the lake bed; from
    underneath it is the sky. A checkerboard rather than the random
    scatter the leaves use, because a regular grid reads as a
    half-transparent sheet where a random one reads as damage.

    The crests stay solid: they are the part the eye reads as a surface,
    and holes through them would make the water look torn."""
    gen = cm_gen(7)
    lum = gen.integers(-6, 7, (B, B)).astype(float)
    crest = np.zeros((B, B), dtype=bool)
    for y in range(0, B, 4):
        off = int(gen.integers(0, B))
        for k in range(5):
            lum[(y + (k % 2)) % B, (off + k) % B] += 26
            crest[(y + (k % 2)) % B, (off + k) % B] = True
    yy, xx = np.mgrid[0:B, 0:B]
    holes = ((xx + yy) % 2 == 0) & ~crest
    return cm_alpha(cm_rgb(lum, (48, 84, 196)), holes)


def cm_log_side():
    """Bark: vertical furrows, v along the trunk."""
    gen = cm_gen(8)
    col = gen.integers(-12, 13, B).astype(float)
    lum = np.tile(col[None, :], (B, 1)) + gen.integers(-6, 7, (B, B))
    for x in gen.choice(B, 4, replace=False):         # deep furrows, broken
        for y in range(B):
            if gen.random() < 0.8:
                lum[y, x] -= 26
    return cm_rgb(lum, (100, 76, 46))


def cm_log_top():
    """The cut end: rings round the centre, bark round the rim."""
    gen = cm_gen(9)
    ys, xs = np.mgrid[0:B, 0:B] + 0.5
    r = np.maximum(np.abs(xs - B / 2), np.abs(ys - B / 2))   # squarish rings
    ring = np.floor(r).astype(int)
    lum = np.where(ring % 2 == 0, 10.0, -8.0) + gen.integers(-5, 6, (B, B))
    img = cm_rgb(lum, (168, 132, 82)).astype(int)
    bark = cm_rgb(gen.integers(-10, 11, (B, B)).astype(float), (100, 76, 46)).astype(int)
    rim = r >= B / 2 - 1
    img[rim] = bark[rim]
    return img.astype(np.uint8)


def cm_planks():
    """Four boards per block, 4 texels each, joints staggered."""
    gen = cm_gen(10)
    lum = gen.integers(-7, 8, (B, B)).astype(float)
    for board in range(4):
        y0 = board * 4
        lum[y0 : y0 + 4, :] += int(gen.integers(-8, 9))
        lum[y0 + 3, :] -= 30                          # the gap under the board
        j = (board * 7 + 3) % B                       # its end joint
        lum[y0 : y0 + 3, j] -= 24
        for x in gen.integers(0, B, 3):               # grain
            lum[y0 + int(gen.integers(0, 3)), x] -= 12
    return cm_rgb(lum, (164, 128, 78))


def cm_planks_lum(tag):
    """The plank pattern on its own, so a block that is MADE of planks
    can put something on top of it rather than inventing its own wood."""
    gen = cm_gen(tag)
    lum = gen.integers(-7, 8, (B, B)).astype(float)
    for board in range(4):
        y0 = board * 4
        lum[y0 : y0 + 4, :] += int(gen.integers(-8, 9))
        lum[y0 + 3, :] -= 30
        j = (board * 7 + 3) % B
        lum[y0 : y0 + 3, j] -= 24
        for x in gen.integers(0, B, 3):
            lum[y0 + int(gen.integers(0, 3)), x] -= 12
    return lum, gen


def cm_table_top():
    """The crafting table seen from above: planks with a 3x3 grid burnt
    into them, which is the one picture of crafting everybody knows --
    even though this game has no grid to fill in (Part C)."""
    lum, _ = cm_planks_lum(30)
    # Two lines each way at thirds of the block, and a border, so the
    # nine cells read at the size a block actually gets drawn.
    for at in (5, 10):
        lum[at, 1:15] -= 46
        lum[1:15, at] -= 46
    lum[0, :] -= 26
    lum[15, :] -= 26
    lum[:, 0] -= 26
    lum[:, 15] -= 26
    return cm_rgb(lum, (164, 128, 78))


def cm_table_side():
    """... and from the side: the same planks with a tool rack on them,
    dark pegs under a rail."""
    lum, gen = cm_planks_lum(31)
    lum[4, 1:15] -= 34                     # the rail
    for x in (3, 7, 11):                   # what hangs off it
        lum[5:9, x] -= 40
        lum[8, x - 1 : x + 2] -= 28
    lum += gen.integers(-4, 5, (B, B))
    return cm_rgb(lum, (152, 118, 72))


def cm_stone_lum(tag):
    """The stone pattern on its own, for blocks BUILT of stone."""
    gen = cm_gen(tag)
    lum = 10 * pnoise(B, B, 1.2, 1.2, gen) + gen.integers(-8, 9, (B, B))
    for _ in range(3):
        x, y = gen.integers(0, B, 2)
        n = int(gen.integers(2, 5))
        for k in range(n):
            lum[y % B, (x + k) % B] -= 20
            y += int(gen.integers(0, 2))
    return lum, gen


def cm_furnace_top():
    """... and its lid, with a rim so the block reads as a box from
    above rather than as a patch of floor."""
    lum, _ = cm_stone_lum(33)
    lum[0:2, :] += 12
    lum[14:16, :] -= 16
    lum[:, 0:2] += 8
    lum[:, 14:16] -= 12
    lum[4:12, 4:12] -= 10
    return cm_rgb(lum, (112, 112, 114))


def cm_furnace_front():
    """The face with the fire in it: an arched opening, three bars
    across, and the dark of the firebox behind them."""
    lum, gen = cm_stone_lum(34)
    # The opening: rows 5..13, inset, with the top two corners cut so it
    # arches rather than sitting there as a rectangle.
    for y in range(5, 14):
        for x in range(3, 13):
            corner = (y == 5 and (x < 5 or x > 10)) or (y == 6 and (x < 4 or x > 11))
            if corner:
                continue
            lum[y, x] = -66 + int(gen.integers(-6, 7))
    for x in range(3, 13):                 # the grate
        for y in (8, 11):
            lum[y, x] += 26
    lum[13, 3:13] += 14                    # the lip it all sits on
    return cm_rgb(lum, (114, 114, 116))


def cm_iron_ore():
    """Stone with pale tan blobs in it, so it reads as ore but not as
    coal -- the two sit next to each other underground."""
    lum, gen = cm_stone_lum(35)
    rgb = cm_rgb(lum, (122, 122, 124))
    for cx, cy, r in ((4, 4, 2), (11, 6, 2), (6, 11, 2), (12, 12, 1)):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r and 0 <= x < B and 0 <= y < B:
                    d = int(gen.integers(-12, 13))
                    rgb[y, x] = np.clip(np.array([206 + d, 168 + d, 132 + d]), 0, 255)
    return rgb


CHEST_IRON = (86, 86, 92)


def cm_chest_side():
    """Planks with two iron bands and a latch: a box, from any side."""
    lum, gen = cm_planks_lum(36)
    rgb = cm_rgb(lum, (150, 110, 62))
    for y in (2, 13):
        rgb[y, :] = CHEST_IRON
        rgb[y + 1, :] = tuple(c - 18 for c in CHEST_IRON)
    # The latch, in the middle of the front.
    for y in range(6, 11):
        for x in range(6, 10):
            rgb[y, x] = CHEST_IRON if (y + x) % 3 else tuple(c + 26 for c in CHEST_IRON)
    return rgb


def cm_chest_top():
    """... and its lid, banded the other way."""
    lum, _ = cm_planks_lum(37)
    rgb = cm_rgb(lum, (146, 106, 58))
    for x in (2, 13):
        rgb[:, x] = CHEST_IRON
        rgb[:, x + 1] = tuple(c - 18 for c in CHEST_IRON)
    return rgb


def cm_trash_side():
    """The same box, in grey with a dark mouth: it is a chest that eats
    what you put in it, and it has to look like it."""
    lum, _ = cm_planks_lum(38)
    rgb = cm_rgb(lum - 18, (104, 100, 98))
    for y in (2, 13):
        rgb[y, :] = (62, 60, 58)
        rgb[y + 1, :] = (48, 46, 44)
    for y in range(6, 12):
        for x in range(4, 12):
            rgb[y, x] = (34, 32, 30)
    return rgb


def cm_trash_top():
    lum, _ = cm_planks_lum(39)
    rgb = cm_rgb(lum - 18, (100, 96, 94))
    for y in range(3, 13):
        for x in range(3, 13):
            rgb[y, x] = (30, 28, 26)
    return rgb


def cm_bench_top():
    """The crafting table's opposite number: the same planks, with the
    grid replaced by a cut across it."""
    lum, gen = cm_planks_lum(41)
    for at in (7, 8):
        lum[at, 1:15] -= 52
    for x in range(2, 14, 2):                # the teeth of the saw
        lum[6, x] -= 34
        lum[9, x + 1] -= 34
    lum += gen.integers(-3, 4, (B, B))
    lum[0, :] -= 26
    lum[15, :] -= 26
    return cm_rgb(lum, (150, 116, 70))


# --- Item icons -------------------------------------------------------
#
# The things that are NOT blocks need a picture of their own: a block can
# be drawn in the inventory with its own texture, and coal cannot. 16x16
# with a cut-out background (alpha < 128 is a hole, se_texture.h), so the
# slot shows through around them.

def _icon():
    return np.zeros((B, B, 4), np.uint8)


def _dot(img, x, y, rgb):
    if 0 <= x < B and 0 <= y < B:
        img[y, x, 0] = rgb[0]
        img[y, x, 1] = rgb[1]
        img[y, x, 2] = rgb[2]
        img[y, x, 3] = 255


def _rect(img, x0, y0, x1, y1, rgb):
    for y in range(y0, y1):
        for x in range(x0, x1):
            _dot(img, x, y, rgb)


def _shade(rgb, d):
    return tuple(int(max(0, min(255, c + d))) for c in rgb)


HANDLE = (138, 98, 56)


def _tool_handle(img):
    """Corner to corner, two texels wide, as every tool has."""
    for i in range(10):
        _dot(img, 3 + i, 13 - i, HANDLE)
        _dot(img, 4 + i, 13 - i, _shade(HANDLE, -26))


def cm_item_pickaxe(rgb):
    img = _icon()
    _tool_handle(img)
    # A wide head with both points turned down.
    for i in range(7):
        _dot(img, 6 + i, 4, rgb)
        _dot(img, 6 + i, 5, _shade(rgb, -22))
    _rect(img, 5, 5, 7, 8, rgb)
    _rect(img, 11, 5, 13, 8, rgb)
    _dot(img, 5, 4, _shade(rgb, 20))
    return img


def cm_item_axe(rgb):
    img = _icon()
    _tool_handle(img)
    # A wedge on one side of the top of the handle.
    for i in range(5):
        _rect(img, 6, 3 + i, 11 - (i // 2), 4 + i, rgb)
    _rect(img, 6, 3, 8, 8, _shade(rgb, 18))
    return img


def cm_item_shovel(rgb):
    img = _icon()
    _tool_handle(img)
    _rect(img, 8, 3, 12, 8, rgb)
    _rect(img, 8, 3, 12, 4, _shade(rgb, 22))
    _rect(img, 8, 7, 12, 8, _shade(rgb, -22))
    return img


def cm_item_ingot():
    """A bar: a flat-topped trapezium with a highlight along the top."""
    img = _icon()
    rgb = (214, 214, 220)
    for i, y in enumerate(range(6, 11)):
        x0 = 3 + i // 2
        x1 = 13 - i // 2
        _rect(img, x0, y, x1, y + 1, _shade(rgb, -6 * i))
    _rect(img, 4, 6, 12, 7, _shade(rgb, 22))
    return img


def cm_item_coal():
    """Three lumps, because one reads as a hole in the slot."""
    img = _icon()
    gen = cm_gen(40)
    for cx, cy, r in ((6, 7, 3), (10, 5, 2), (10, 10, 2)):
        for y in range(cy - r, cy + r + 1):
            for x in range(cx - r, cx + r + 1):
                if (x - cx) ** 2 + (y - cy) ** 2 <= r * r:
                    lum = int(gen.integers(-10, 11))
                    _dot(img, x, y, _shade((44, 44, 50), lum))
    _dot(img, 5, 6, (96, 96, 104))
    _dot(img, 9, 4, (86, 86, 94))
    return img


def cm_item_stick():
    img = _icon()
    for i in range(11):
        _dot(img, 3 + i, 13 - i, HANDLE)
        _dot(img, 4 + i, 13 - i, _shade(HANDLE, -30))
    _dot(img, 3, 14, _shade(HANDLE, -30))
    return img


def cm_alpha(rgb, holes):
    """RGB plus a hole mask -> RGBA: the engine draws alpha < 128 as a
    hole (cut-out transparency), everything else opaque."""
    a = np.where(holes, 0, 255).astype(np.uint8)[:, :, None]
    return np.concatenate([rgb, a], axis=2)


def cm_leaves_lum():
    gen = cm_gen(11)
    lum = gen.integers(-18, 19, (B, B)).astype(float)
    holes = gen.random((B, B)) < 0.22
    return lum, holes


def cm_leaves():
    """Leaves with gaps (cut-out): the sky and the branches behind show
    through, as in Minecraft's "fancy" leaves. For canopies close by."""
    lum, holes = cm_leaves_lum()
    return cm_alpha(cm_rgb(lum, (58, 112, 38)), holes)


def cm_leaves_fast():
    """The same leaves, opaque: the gaps dark ("fast" leaves), for the
    textured canopies further off, which are drawn without their
    insides."""
    lum, holes = cm_leaves_lum()
    lum[holes] = -52
    return cm_rgb(lum, (58, 112, 38))


def cm_birch_leaves_lum():
    gen = cm_gen(42)
    lum = gen.integers(-16, 17, (B, B)).astype(float)
    holes = gen.random((B, B)) < 0.24
    return lum, holes


def cm_birch_leaves():
    """Lighter and yellower than oak: what makes a birch wood read as a
    different wood from across a valley."""
    lum, holes = cm_birch_leaves_lum()
    return cm_alpha(cm_rgb(lum, (108, 152, 62)), holes)


def cm_birch_leaves_fast():
    lum, holes = cm_birch_leaves_lum()
    lum[holes] = -48
    return cm_rgb(lum, (108, 152, 62))


def cm_birch_side():
    """White bark with the dark scars birches have, which is the whole
    signature of the tree."""
    gen = cm_gen(43)
    lum = gen.integers(-7, 8, (B, B)).astype(float)
    for _ in range(5):                       # the scars, short and level
        y = int(gen.integers(0, B))
        x = int(gen.integers(0, B))
        n = int(gen.integers(2, 5))
        for k in range(n):
            lum[y, (x + k) % B] -= 58
            if gen.random() < 0.4:
                lum[(y + 1) % B, (x + k) % B] -= 34
    for x in range(B):                       # a faint vertical grain
        lum[:, x] += int(gen.integers(-5, 6))
    return cm_rgb(lum, (216, 214, 202))


def cm_birch_top():
    """The cut end: rings, like the oak's but paler."""
    gen = cm_gen(44)
    ys, xs = np.mgrid[0:B, 0:B] + 0.5
    d = np.sqrt((xs - B / 2) ** 2 + (ys - B / 2) ** 2)
    lum = (np.sin(d * 1.9) * 9.0) + gen.integers(-6, 7, (B, B))
    lum[d > 7.1] -= 26                       # the bark round the edge
    return cm_rgb(lum, (196, 184, 156))


def cm_cactus():
    """Green with vertical ribs and a paler edge, so a stack of them
    still reads as separate blocks."""
    gen = cm_gen(45)
    lum = gen.integers(-6, 7, (B, B)).astype(float)
    for x in range(1, B, 4):                 # the ribs
        lum[:, x] -= 26
        lum[:, (x + 1) % B] += 10
    lum[0, :] += 16
    lum[15, :] -= 22
    for _ in range(14):                      # spines
        lum[int(gen.integers(0, B)), int(gen.integers(0, B))] += 40
    return cm_rgb(lum, (74, 128, 60))


def cm_snow():
    """Nearly white, and nearly flat: snow has no features, and any it
    is given read as dirt on it."""
    gen = cm_gen(46)
    lum = gen.integers(-5, 6, (B, B)).astype(float)
    lum += 6.0 * pnoise(B, B, 2.6, 2.6, gen)
    return cm_rgb(lum, (236, 240, 248))


def cm_sandstone():
    """Sand, pressed: the same colour with level bedding lines through
    it, which is what tells the two apart underground."""
    gen = cm_gen(47)
    lum = gen.integers(-5, 6, (B, B)).astype(float)
    lum += 4.0 * pnoise(B, B, 3.0, 3.0, gen)
    for y in (3, 7, 12):                     # bedding
        lum[y, :] -= 20
        lum[(y + 1) % B, :] += 7
    return cm_rgb(lum, (214, 198, 148))


def cm_coal_ore():
    img = cm_stone().astype(int)
    gen = cm_gen(12)
    lump = [(0, 0), (1, 0), (0, 1), (1, 1), (2, 1), (1, 2), (-1, 1), (2, 0)]
    for _ in range(5):                                # black lumps of 5-8 texels
        x, y = (int(v) for v in gen.integers(0, B, 2))
        for dx, dy in lump[: int(gen.integers(5, 9))]:
            img[(y + dy) % B, (x + dx) % B] = (26 + gen.integers(0, 18),) * 3
    return img.astype(np.uint8)


def cm_face():
    """The miner's face, the head's front: skin, dark eyes with a white
    glint, bushy brows, a big brown moustache. v = 0 is the top."""
    gen = cm_gen(13)
    skin = (222, 170, 128)
    lum = gen.integers(-5, 6, (B, B)).astype(float)
    img = cm_rgb(lum, skin).astype(int)
    brow, hair, eye_w, eye_d, mouth = (70, 46, 28), (96, 62, 36), (240, 240, 236), (40, 44, 70), (150, 80, 70)
    img[0:3, :] = hair                                # hair under the hat's brim
    img[5, 2:7] = brow
    img[5, 9:14] = brow
    img[7, 3:6] = eye_w
    img[7, 10:13] = eye_w
    img[7, 4] = eye_d
    img[7, 11] = eye_d
    img[8, 3:6] = eye_d
    img[8, 10:13] = eye_d
    img[9:11, 7:9] = (200, 140, 104)                  # the nose, shaded
    img[11, 3:13] = hair                              # the moustache
    img[12, 2:6] = hair
    img[12, 10:14] = hair
    img[13, 6:10] = mouth
    return np.clip(img, 0, 255).astype(np.uint8)


def cm_sprite(tag, stem, bloom):
    """A plant on a cross of two quads (cut-out): a green stem with two
    leaves, and a bloom of `bloom` colours on top; the rest is holes."""
    gen = cm_gen(tag)
    rgb = np.zeros((B, B, 3), np.uint8)
    holes = np.ones((B, B), bool)

    def px(x, y, c):
        rgb[y, x] = np.clip(np.array(c) + gen.integers(-10, 11, 3), 0, 255)
        holes[y, x] = False

    for y in range(7, B):
        px(7, y, stem)
    for k in range(3):                                # the two leaves
        px(6 - k, 12 - k, stem)
        px(8 + k, 11 - k, stem)
    for dy, row in enumerate(bloom):
        for dx, c in enumerate(row):
            if c is not None:
                px(5 + dx, 2 + dy, c)
    return cm_alpha(rgb, holes)


R, Y, W, K = (214, 40, 36), (250, 212, 40), (250, 244, 214), (96, 40, 20)


def cm_flower_red():
    return cm_sprite(14, (60, 130, 40), [[None, R, R, R, None], [R, R, K, R, R], [R, K, Y, K, R], [R, R, K, R, R],
                                          [None, R, R, R, None]])


def cm_flower_yellow():
    return cm_sprite(15, (70, 140, 44), [[None, None, Y, None, None], [None, Y, Y, Y, None], [Y, Y, W, Y, Y],
                                          [None, Y, Y, Y, None], [None, None, Y, None, None]])


def cm_tall_grass():
    """Tufts of grass blades (cut-out), for crossed quads."""
    gen = cm_gen(16)
    rgb = np.zeros((B, B, 3), np.uint8)
    holes = np.ones((B, B), bool)
    for x in range(1, B - 1):
        if gen.random() < 0.35:
            continue
        top = int(gen.integers(3, 12))
        lean = int(gen.integers(-1, 2))
        for y in range(top, B):
            xx = x + (lean if y < top + 3 else 0)
            rgb[y, xx] = np.clip(np.array(GRASS) + gen.integers(-22, 12, 3) - 6 * (B - y) // 4, 0, 255)
            holes[y, xx] = False
    return cm_alpha(rgb, holes)


def cm_glass():
    """A window pane (cut-out): a frame round the edge and a few white
    glints; the rest is holes -- no blending, so clear glass is empty."""
    gen = cm_gen(17)
    rgb = np.zeros((B, B, 3), np.uint8)
    holes = np.ones((B, B), bool)
    frame = (200, 216, 222)
    for i in range(B):
        for x, y in ((i, 0), (i, B - 1), (0, i), (B - 1, i)):
            rgb[y, x] = frame
            holes[y, x] = False
    for x, y in ((4, 3), (3, 4), (5, 3), (3, 5), (11, 9), (10, 10), (9, 11)):
        rgb[y, x] = (236, 244, 248)
        holes[y, x] = False
    return cm_alpha(rgb, holes)


def cm_torch():
    """The torch's stick: dark wood, with a glowing coal band at the top
    (the flame itself is the shared flame asset)."""
    gen = cm_gen(18)
    lum = gen.integers(-10, 11, (B, B)).astype(float)
    img = cm_rgb(lum, (110, 80, 44)).astype(int)
    img[0:3, :] = (255, 214, 96)
    img[3, :] = (200, 110, 40)
    return img.astype(np.uint8)


def cm_bedrock():
    """Beta's bedrock: dark grey, blotched near-black and pale grey at
    random, with no pattern to it."""
    gen = cm_gen(19)
    lum = gen.integers(-10, 11, (B, B)).astype(float)
    pick = gen.random((B, B))
    lum[pick < 0.30] -= 34                            # near-black patches
    lum[pick > 0.82] += 40                            # pale chips
    lum = lum + 8 * pnoise(B, B, 1.0, 1.0, gen)
    return cm_rgb(lum, (84, 84, 84))


def cm_gravel():
    """Pebbles in grey, brown-grey and near-white, packed tight: each
    texel belongs to its nearest pebble (wrapping, so it tiles), darker at
    the pebble's rim."""
    gen = cm_gen(20)
    n = 22
    seeds = gen.uniform(0, B, (n, 2))
    tints = np.array([(128, 122, 118), (104, 98, 94), (150, 142, 136), (92, 84, 80), (170, 164, 160)])
    tint = tints[gen.integers(0, len(tints), n)]
    ys, xs = np.mgrid[0:B, 0:B] + 0.5
    d = []
    for sx, sy in seeds:
        dx = np.abs(xs - sx)
        dy = np.abs(ys - sy)
        dx = np.minimum(dx, B - dx)
        dy = np.minimum(dy, B - dy)
        d.append(np.sqrt(dx * dx + dy * dy))
    d = np.array(d)
    order = np.sort(d, axis=0)
    cell = np.argmin(d, axis=0)
    img = tint[cell].astype(float)
    img += (gen.integers(-8, 9, (B, B)) - 3.0 * order[0])[:, :, None]
    img[(order[1] - order[0]) < 0.6] *= 0.55          # the gaps between pebbles
    return np.clip(img, 0, 255).astype(np.uint8)


# A 5x7 pixel font for the sign texts, one string of 7 rows of 5 per
# glyph ('#' ink). Written out here rather than taken from PIL, so the
# PNGs come out the same whatever PIL is installed. Only the letters the
# texts use.
FONT5x7 = {
    " ": [".....", ".....", ".....", ".....", ".....", ".....", "....."],
    "!": ["..#..", "..#..", "..#..", "..#..", "..#..", ".....", "..#.."],
    "B": ["####.", "#...#", "#...#", "####.", "#...#", "#...#", "####."],
    "F": ["#####", "#....", "#....", "####.", "#....", "#....", "#...."],
    "K": ["#...#", "#..#.", "#.#..", "##...", "#.#..", "#..#.", "#...#"],
    "L": ["#....", "#....", "#....", "#....", "#....", "#....", "#####"],
    "W": ["#...#", "#...#", "#...#", "#.#.#", "#.#.#", "##.##", "#...#"],
    "a": [".....", ".....", ".###.", "....#", ".####", "#...#", ".####"],
    "d": ["....#", "....#", ".##.#", "#..##", "#...#", "#...#", ".####"],
    "e": [".....", ".....", ".###.", "#...#", "#####", "#....", ".###."],
    "f": ["..##.", ".#..#", ".#...", "###..", ".#...", ".#...", ".#..."],
    "h": ["#....", "#....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "i": ["..#..", ".....", ".##..", "..#..", "..#..", "..#..", ".###."],
    "l": [".##..", "..#..", "..#..", "..#..", "..#..", "..#..", ".###."],
    "n": [".....", ".....", "#.##.", "##..#", "#...#", "#...#", "#...#"],
    "o": [".....", ".....", ".###.", "#...#", "#...#", "#...#", ".###."],
    "r": [".....", ".....", "#.##.", "##..#", "#....", "#....", "#...."],
    "s": [".....", ".....", ".####", "#....", ".###.", "....#", "####."],
    "t": [".#...", ".#...", "###..", ".#...", ".#...", ".#..#", "..##."],
    "u": [".....", ".....", "#...#", "#...#", "#...#", "#..##", ".##.#"],
    "w": [".....", ".....", "#...#", "#...#", "#.#.#", "#.#.#", ".#.#."],
}

# The sign texts, in VM_SIGN_0.. order (voxel_mesh.h lists them too):
# two lines each, at most 10 characters a line (6 texels each on 64).
SIGN_TEXTS = [("Kurt", "was here"), ("Wolfie", "was here"), ("Far Lands", "or Bust!")]


def cm_sign(lines):
    """A sign's front, 64x32 across a board 1 block wide and 1/2 high:
    the planks at four texels to one of theirs -- the same boards as the
    planks block, so it matches the rest of the sign -- with the text
    in dark ink, centred, two lines."""
    planks = cm_planks()
    img = np.repeat(np.repeat(planks, 4, axis=0), 4, axis=1)[:32, :64].astype(int)
    img[0, :] = img[-1, :] = (70, 50, 28)             # the board's edge
    img[:, 0] = img[:, -1] = (70, 50, 28)
    ink = (38, 26, 14)
    for row, text in enumerate(lines):
        width = 6 * len(text) - 1
        x0 = (64 - width) // 2
        y0 = 7 + row * 11
        for k, ch in enumerate(text):
            glyph = FONT5x7[ch]
            for gy in range(7):
                for gx in range(5):
                    if glyph[gy][gx] == "#":
                        img[y0 + gy, x0 + 6 * k + gx] = ink
    return img.astype(np.uint8)


def cm_torch_flame():
    """Torch flame, 64x8 like flame(): white-yellow core -> orange ->
    red tip."""
    w = 64
    stops = [(0.00, (255, 252, 214)), (0.20, (255, 222, 110)), (0.50, (255, 150, 40)),
             (0.80, (220, 70, 20)), (1.00, (120, 24, 8))]
    xs = np.linspace(0.0, 1.0, w)
    ramp = np.zeros((w, 3))
    for c in range(3):
        ramp[:, c] = np.interp(xs, [p for p, _ in stops], [col[c] for _, col in stops])
    streak = np.array([1.00, 0.92, 1.05, 0.95, 1.00, 0.90, 1.04, 0.96])[:, None, None]
    fade = np.clip((xs - 0.15) / 0.3, 0.0, 1.0)[None, :, None]
    img = ramp[None, :, :] * (1.0 + (streak - 1.0) * fade)
    return np.clip(img, 0, 255).astype(np.uint8)


# Keyed by the path under textures/. The generators share their seeded
# random streams in this order, so keep it: a reordered entry changes
# every texture after it.
TEXTURES = {
    "grass_top.png": cm_grass_top,
    "grass_side.png": cm_grass_side,
    "dirt.png": cm_dirt,
    "stone.png": cm_stone,
    "cobble.png": cm_cobble,
    "sand.png": cm_sand,
    "water.png": cm_water,
    "log_side.png": cm_log_side,
    "log_top.png": cm_log_top,
    "planks.png": cm_planks,
    "leaves.png": cm_leaves,
    "coal_ore.png": cm_coal_ore,
    "miner_face.png": cm_face,
    "torch_flame.png": cm_torch_flame,
    "flower_red.png": cm_flower_red,
    "flower_yellow.png": cm_flower_yellow,
    "tall_grass.png": cm_tall_grass,
    "glass.png": cm_glass,
    "torch.png": cm_torch,
    "leaves_fast.png": cm_leaves_fast,
    "bedrock.png": cm_bedrock,
    "gravel.png": cm_gravel,
    "iron_ore.png": cm_iron_ore,
    "birch_side.png": cm_birch_side,
    "birch_top.png": cm_birch_top,
    "birch_leaves.png": cm_birch_leaves,
    "birch_leaves_fast.png": cm_birch_leaves_fast,
    "cactus.png": cm_cactus,
    "snow.png": cm_snow,
    "sandstone.png": cm_sandstone,
    "chest_top.png": cm_chest_top,
    "chest_side.png": cm_chest_side,
    "trash_top.png": cm_trash_top,
    "trash_side.png": cm_trash_side,
    "bench_top.png": cm_bench_top,
    "item_coal.png": cm_item_coal,
    "item_iron_ingot.png": cm_item_ingot,
    "item_pickaxe_iron.png": lambda: cm_item_pickaxe((222, 222, 228)),
    "item_axe_iron.png": lambda: cm_item_axe((222, 222, 228)),
    "item_shovel_iron.png": lambda: cm_item_shovel((222, 222, 228)),
    "item_stick.png": cm_item_stick,
    "item_pickaxe_wood.png": lambda: cm_item_pickaxe((176, 128, 64)),
    "item_pickaxe_stone.png": lambda: cm_item_pickaxe((144, 152, 160)),
    "item_axe_wood.png": lambda: cm_item_axe((192, 136, 72)),
    "item_axe_stone.png": lambda: cm_item_axe((160, 168, 176)),
    "item_shovel_wood.png": lambda: cm_item_shovel((160, 120, 56)),
    "item_shovel_stone.png": lambda: cm_item_shovel((136, 143, 152)),
    "furnace_front.png": cm_furnace_front,
    "furnace_top.png": cm_furnace_top,
    "table_top.png": cm_table_top,
    "table_side.png": cm_table_side,
    "sign_kurt.png": lambda: cm_sign(SIGN_TEXTS[0]),
    "sign_wolfie.png": lambda: cm_sign(SIGN_TEXTS[1]),
    "sign_flob.png": lambda: cm_sign(SIGN_TEXTS[2]),
}


def main():
    tiles = []
    for name, fn in TEXTURES.items():
        img = fn()
        (OUT / name).parent.mkdir(parents=True, exist_ok=True)
        Image.fromarray(img, "RGBA" if img.shape[2] == 4 else "RGB").save(OUT / name, optimize=True)
        # The contact sheet shows holes in magenta, like the engine's key.
        if img.shape[2] == 4:
            img = np.where(img[:, :, 3:] < 128, np.array([255, 0, 255], np.uint8), img[:, :, :3])
        tiles.append(img)
        print(f"wrote {OUT / name}")
    if "--preview" in sys.argv:
        # Each texture repeated 2x2 (so seams and wrap-around are visible)
        # and scaled 4x, side by side.
        # Each texture 2x2, padded to the tallest so they sit side by side.
        tall = max(t.shape[0] for t in tiles) * 2
        blocks = [np.tile(t, (2, 2, 1)) for t in tiles]
        blocks = [np.pad(b, ((0, tall - b.shape[0]), (0, 4), (0, 0))) for b in blocks]
        row = np.concatenate(blocks, axis=1)
        prev = Image.fromarray(row, "RGB").resize((row.shape[1] * 4, row.shape[0] * 4), Image.NEAREST)
        path = Path(sys.argv[sys.argv.index("--preview") + 1]) if len(sys.argv) > sys.argv.index("--preview") + 1 \
            else OUT.parent / "build" / "textures_preview.png"
        path.parent.mkdir(parents=True, exist_ok=True)
        prev.save(path)
        print(f"wrote {path}")


if __name__ == "__main__":
    main()
