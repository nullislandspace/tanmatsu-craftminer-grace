#!/usr/bin/env python3
# =====================================================================
#  CraftMiner  --  fetch and verify the soundtrack
# ---------------------------------------------------------------------
#  The music in assets/music comes from the Mutopia Project, and the
#  rule it has to keep is in assets/music/MUSIC.md: a MIDI file carries
#  the copyright of its ENGRAVING as well as of the composition, so only
#  files Mutopia marks "Public Domain" may ship. Mutopia's Creative
#  Commons material -- which includes pieces we would otherwise want,
#  such as the Gnossiennes -- is share-alike, and would put conditions
#  on anyone redistributing the game.
#
#  This script is what keeps that rule from quietly lapsing. It REFUSES
#  to download anything that is not Public Domain, whatever is asked
#  for, so extending the set cannot go wrong by accident.
#
#      python3 tools/get_music.py             verify the shipped files
#      python3 tools/get_music.py --fetch     download them again
#      python3 tools/get_music.py --survey    what else Mutopia has
#      python3 tools/get_music.py --doc       rewrite MUSIC.md
#
#  Needs the network. Nothing in the build depends on it: the files are
#  committed, and `make check` reads them from the repository.
# =====================================================================

import argparse
import hashlib
import html
import json
import os
import re
import sys
import time
import urllib.request

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MUSIC_DIR = os.path.join(ROOT, "assets", "music")
MANIFEST = os.path.join(MUSIC_DIR, "manifest.json")

# The composers worth looking at for this game: quiet, mostly solo
# piano, and dead long enough that the composition is unarguable.
COMPOSERS = ["SatieE", "SchumannR", "DebussyC", "ChopinFF", "BachJS"]

UA = {"User-Agent": "craftminer-music/1.0 (+https://github.com/cavac)"}


def get(url):
    return urllib.request.urlopen(urllib.request.Request(url, headers=UA), timeout=60).read()


ROW = re.compile(r'<table class="table-bordered result-table">(.*?)</table>', re.S)


def parse_page(page):
    out = []
    for block in ROW.findall(page):
        tds = [html.unescape(re.sub("<[^>]+>", "", t)).strip()
               for t in re.findall(r"<td[^>]*>(.*?)</td>", block, re.S)]
        mid = re.search(r'href="(https://[^"]+\.mid)"', block)
        info = re.search(r"piece-info\.cgi\?id=(\d+)", block)
        if not mid:
            continue
        if "publicdomain" in block:
            lic = "Public Domain"
        elif "ShareAlike" in block:
            lic = "CC BY-SA 4.0"
        elif "Creative Commons" in block:
            lic = "CC BY 4.0"
        else:
            lic = "?"
        out.append({
            "title": tds[0] if tds else "",
            "by": (tds[1] if len(tds) > 1 else "").replace("by ", "").strip(),
            "inst": tds[4] if len(tds) > 4 else "",
            "source": tds[8] if len(tds) > 8 else "",
            "lic": lic,
            "mid": mid.group(1),
            "info": "https://www.mutopiaproject.org/cgibin/piece-info.cgi?id=" +
                    (info.group(1) if info else ""),
        })
    return out


def catalogue(composers):
    rows = []
    for c in composers:
        start = 0
        while True:
            url = ("https://www.mutopiaproject.org/cgibin/make-table.cgi"
                   f"?Composer={c}&startat={start}")
            page = get(url).decode("utf-8", "replace")
            got = parse_page(page)
            if not got:
                break
            rows += got
            if len(got) < 10:
                break
            start += 10
            time.sleep(0.3)
    return rows


def load_manifest():
    with open(MANIFEST, encoding="utf-8") as f:
        return json.load(f)


def fetch_one(entry, rows):
    """Download one manifest entry. Refuses anything that is not PD."""
    hits = [r for r in rows if r["title"] == entry["title"]]
    if not hits:
        raise SystemExit(f"{entry['file']}: Mutopia no longer lists {entry['title']!r}")
    row = hits[0]
    if row["lic"] != "Public Domain":
        raise SystemExit(
            f"{entry['file']}: {entry['title']!r} is now {row['lic']}, not Public Domain. "
            "REFUSING to download it -- see assets/music/MUSIC.md.")
    data = get(row["mid"])
    if data[:4] != b"MThd":
        raise SystemExit(f"{entry['file']}: what came back is not a MIDI file")
    return data, row


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--fetch", action="store_true", help="download the shipped set again")
    ap.add_argument("--survey", action="store_true", help="list what Mutopia has, with licences")
    ap.add_argument("--doc", action="store_true", help="rewrite assets/music/MUSIC.md")
    args = ap.parse_args()

    if args.survey:
        rows = catalogue(COMPOSERS)
        for r in sorted(rows, key=lambda r: (r["lic"] != "Public Domain", r["by"], r["title"])):
            mark = "  " if r["lic"] == "Public Domain" else "no"
            print(f"{mark} {r['lic']:<13} {r['inst'][:20]:<21} {r['by'][:22]:<23} {r['title']}")
        pd = sum(1 for r in rows if r["lic"] == "Public Domain")
        print(f"\n{pd} of {len(rows)} are Public Domain and may be used.", file=sys.stderr)
        return 0

    manifest = load_manifest()
    rows = catalogue(sorted({e["composer_key"] for e in manifest}))

    bad = 0
    for entry in manifest:
        path = os.path.join(MUSIC_DIR, entry["file"])
        data, row = fetch_one(entry, rows)
        digest = hashlib.sha256(data).hexdigest()

        if args.fetch:
            with open(path, "wb") as f:
                f.write(data)
            entry["sha256"] = digest
            entry["bytes"] = len(data)
            print(f"fetched {entry['file']} ({len(data)} bytes)")
            continue

        if not os.path.exists(path):
            print(f"MISSING {entry['file']}")
            bad += 1
            continue
        with open(path, "rb") as f:
            have = f.read()
        if hashlib.sha256(have).hexdigest() != digest:
            print(f"DIFFERS {entry['file']}: Mutopia's copy has changed since it was added")
            bad += 1
        else:
            print(f"ok      {entry['file']} ({len(have)} bytes, {row['lic']})")

    if args.fetch:
        with open(MANIFEST, "w", encoding="utf-8") as f:
            json.dump(manifest, f, indent=1, ensure_ascii=False)
            f.write("\n")

    if args.doc:
        write_doc(manifest)

    return 1 if bad else 0


DOC_HEAD = open(os.path.join(MUSIC_DIR, "MUSIC.md"), encoding="utf-8").read().split(
    "## What is here")[0] if os.path.exists(os.path.join(MUSIC_DIR, "MUSIC.md")) else ""


def write_doc(manifest):
    """Rewrite the table at the end of MUSIC.md; the prose above it stays."""
    out = [DOC_HEAD, "## What is here\n\n",
           "| File | Piece | Composer | Bytes |\n|---|---|---|---|\n"]
    for e in sorted(manifest, key=lambda e: e["file"]):
        out.append(f"| `{e['file']}` | {e['title']} | {e['composer']} | {e['bytes']:,} |\n")
    out.append("\n### Where each one came from\n\n")
    for e in sorted(manifest, key=lambda e: e["file"]):
        out.append(f"**`{e['file']}`** — {e['title']}, {e['composer']}. Public Domain.  \n")
        out.append(f"Mutopia: <{e['info']}>  \n")
        out.append(f"File: <{e['url']}>  \n")
        if e.get("source"):
            out.append(f"Engraved from: {e['source']}\n")
        out.append("\n")
    out.append(DOC_TAIL)
    with open(os.path.join(MUSIC_DIR, "MUSIC.md"), "w", encoding="utf-8") as f:
        f.write("".join(out))
    print("rewrote assets/music/MUSIC.md")


DOC_TAIL = """## The synth, and what it does to these

They are played by six voice shapes, not a General MIDI sound module:
a struck string, a plucked one, a sustained pad, a bass, a reed and a
bell, chosen so the piano repertoire comes out recognisable. A file that
leans on a specific GM patch will sound like something else. That is the
price of a synthesiser measured in kilobytes rather than a sample set
measured in megabytes, and for Satie and Schumann it is not a high one.
"""


if __name__ == "__main__":
    sys.exit(main())
