#!/usr/bin/env python3
"""Build a trimmed CoD2 MP-only IWD pack for the web client.

Drops SP campaign maps, video, demos, and SP voiceovers. Keeps MP maps +
shared assets + MP/battlechatter localization.

Default layout (4 IWDs total — balanced for download + index speed):
  iw_00.iwd, iw_01.iwd
  localized_english_iw00.iwd, localized_english_iw01.iwd

Example:
  python tools/build_web_mp_pack.py ^
    "C:\\Program Files (x86)\\Activision\\Call of Duty 22" ^
    -o "%USERPROFILE%\\Desktop\\cod2-web-mp-pack"
"""
from __future__ import annotations

import argparse
import os
import sys
import zipfile
from typing import Iterable


MAX_PACKED = 150 * 1024 * 1024  # legacy default; prefer --base-packs/--loc-packs

# Default layout: 2 base + 2 localized = 4 IWDs (~600–750 MB each).
DEFAULT_BASE_PACKS = 2
DEFAULT_LOC_PACKS = 2

# SP campaign map / mission tokens (not under maps/mp/).
SP_MAP_TOKENS = (
    "88ridge", "beltot", "bergstein", "breakout", "cityhall", "credits",
    "crossroads", "decoytown", "decoytrenches", "demolition", "downtown_assault",
    "downtown_sniper", "duhoc_assault", "duhoc_defend", "duhoc", "elalamein",
    "eldaba", "hill400_assault", "hill400_defend", "hill400", "libya", "matmata",
    "moscow", "movie_africa", "movie_eastern", "movie_europe", "movie_victory",
    "newvillers", "rhine", "silotown_assault", "silotown", "slideshow_bergstein",
    "slideshow_caen", "slideshow_earlygrad", "slideshow_lategrad", "slideshow_libya",
    "slideshow_rhine", "slideshow_tunisia", "slideshow", "tankhunt", "toujane_ride",
    "toujane", "trainyard",
)


def normalize(name: str) -> str:
    return name.replace("\\", "/").lstrip("/")


def _has_sp_token(path: str) -> bool:
    """True if path looks SP-mission-specific (avoid matching mp_<same>)."""
    p = path.lower()
    # Always keep MP paths.
    if "/mp/" in p or p.startswith("maps/mp/") or "/voiceovers/mp/" in p:
        return False
    for tok in SP_MAP_TOKENS:
        # Require token as path segment or filename stem piece, not inside mp_*.
        if f"mp_{tok}" in p:
            continue
        if f"/{tok}/" in p or f"/{tok}." in p or f"/{tok}_" in p:
            return True
        base = p.rsplit("/", 1)[-1]
        if base.startswith(tok + ".") or base.startswith(tok + "_") or base == tok:
            return True
    return False


def should_keep(path: str) -> bool:
    p = normalize(path).lower()
    if not p or p.endswith("/"):
        return False

    # --- always drop ---
    if p.startswith("maps/") and not p.startswith("maps/mp/"):
        return False
    if p.startswith("video/") or p.startswith("movies/"):
        return False
    if p.startswith("demos/"):
        return False

    # Localized SP campaign dialogue (US/UK/RU mission folders, slideshows).
    # Keep: Battlechatter (shared), Voiceovers/MP, Voiceovers/Generic.
    if p.startswith("sound/voiceovers/"):
        if p.startswith("sound/voiceovers/mp/") or p.startswith("sound/voiceovers/generic/"):
            return True
        return False

    # Mission names also occur in shared MP materials, models, animations and
    # sound aliases (e.g. materials/toujane_brick2). Only apply the name
    # heuristic to campaign strings; asset dependencies cannot be inferred
    # from their names. Campaign maps/video/dialogue were filtered above.
    if p.startswith("localizedstrings/") and _has_sp_token(p):
        return False

    return True


def source_iwds(main_dir: str) -> list[str]:
    names = []
    for name in sorted(os.listdir(main_dir)):
        lower = name.lower()
        if not lower.endswith(".iwd"):
            continue
        if name.startswith("~") or name.startswith("."):
            continue
        # Base game + english localization (+ tiny CoD2x). Skip custom maps/mods.
        if lower.startswith("iw_") or lower.startswith("localized_english_"):
            names.append(os.path.join(main_dir, name))
    return names


def is_localized_path(path: str) -> bool:
    p = normalize(path).lower()
    # Localized packs mostly hold sound/ + localizedstrings/; keep them in
    # localized_english_*.iwd so FS language search works.
    return (
        p.startswith("sound/")
        or p.startswith("localizedstrings/")
        or p.startswith("soundaliases/")
        or "/localized" in p
    )


class PackWriter:
    """Write items into exactly `num_packs` IWDs, split by uncompressed bytes."""

    def __init__(self, out_main: str, prefix: str, num_packs: int):
        self.out_main = out_main
        self.prefix = prefix
        self.num_packs = max(1, int(num_packs))
        self.outputs: list[str] = []

    def write_all(self, items: list[tuple[str, bytes]]) -> None:
        if not items:
            return
        total = sum(len(data) for _arc, data in items)
        # Soft target per pack; last pack absorbs remainder.
        target = max(1, (total + self.num_packs - 1) // self.num_packs)

        packs: list[list[tuple[str, bytes]]] = [[] for _ in range(self.num_packs)]
        sizes = [0] * self.num_packs
        idx = 0
        for arc, data in items:
            # Advance to next pack when current is full (except last).
            if (
                idx < self.num_packs - 1
                and sizes[idx] > 0
                and sizes[idx] + len(data) > target
            ):
                idx += 1
            packs[idx].append((arc, data))
            sizes[idx] += len(data)

        for i, chunk in enumerate(packs):
            if not chunk:
                continue
            name = f"{self.prefix}{i:02d}.iwd"
            path = os.path.join(self.out_main, name)
            print(f"  writing {name} ({len(chunk)} files, "
                  f"{sizes[i] / (1024 * 1024):.1f} MB uncompressed) …", flush=True)
            with zipfile.ZipFile(
                path,
                "w",
                compression=zipfile.ZIP_DEFLATED,
                compresslevel=1,
                allowZip64=True,
            ) as zf:
                for arc, data in chunk:
                    zf.writestr(arc, data, compress_type=zipfile.ZIP_DEFLATED)
            disk = os.path.getsize(path)
            print(f"    -> {name} {disk / (1024 * 1024):.1f} MB on disk", flush=True)
            self.outputs.append(path)


def iter_kept_members(paths: Iterable[str]):
    seen: set[str] = set()
    skipped = 0
    dupes = 0
    # The engine searches base IWDs before localized IWDs, and higher names
    # before lower names within either group. Resolve duplicates in that same
    # order so iw_15's 1.3 scripts replace the older copies in iw_07.
    def precedence(path: str):
        name = os.path.basename(path).lower()
        return (not name.startswith("localized_"), name)

    for path in sorted(paths, key=precedence, reverse=True):
        print(f"scanning {os.path.basename(path)} …", flush=True)
        try:
            zf = zipfile.ZipFile(path)
        except zipfile.BadZipFile as exc:
            print(f"  skip bad zip: {exc}", flush=True)
            continue
        with zf:
            for info in zf.infolist():
                if info.is_dir():
                    continue
                arc = normalize(info.filename)
                if not should_keep(arc):
                    skipped += 1
                    continue
                key = arc.lower()
                if key in seen:
                    dupes += 1
                    continue
                seen.add(key)
                data = zf.read(info)
                yield arc, data
    print(f"unique kept={len(seen)} skipped_filter={skipped} skipped_dupes={dupes}", flush=True)


def build(install_root: str, out_root: str, base_packs: int, loc_packs: int) -> int:
    main_in = os.path.join(install_root, "main")
    if not os.path.isdir(main_in):
        print(f"error: missing {main_in}", file=sys.stderr)
        return 1

    out_main = os.path.join(out_root, "main")
    os.makedirs(out_main, exist_ok=True)
    for name in os.listdir(out_main):
        if name.lower().endswith(".iwd"):
            os.remove(os.path.join(out_main, name))

    sources = source_iwds(main_in)
    if not sources:
        print("error: no source IWDs", file=sys.stderr)
        return 1

    priority: list[tuple[str, bytes]] = []
    base_items: list[tuple[str, bytes]] = []
    loc_items: list[tuple[str, bytes]] = []

    for arc, data in iter_kept_members(sources):
        low = arc.lower()
        if low in ("default_mp.cfg", "default.cfg", "configure_mp.cfg", "configure_mp.csv"):
            priority.append((arc, data))
            continue
        if is_localized_path(arc) and not low.endswith((".cfg", ".csv")):
            loc_items.append((arc, data))
        else:
            base_items.append((arc, data))

    # Critical cfgs always first in iw_00.
    base_items = priority + base_items

    print(f"splitting into {base_packs} base + {loc_packs} localized IWD(s)…", flush=True)
    base = PackWriter(out_main, "iw_", base_packs)
    loc = PackWriter(out_main, "localized_english_iw", loc_packs)
    base.write_all(base_items)
    loc.write_all(loc_items)

    if not loc.outputs:
        print("warning: no localized_english output — menus may fail", flush=True)

    total = sum(os.path.getsize(p) for p in base.outputs + loc.outputs)
    print(f"\nDone. {len(base.outputs)} base + {len(loc.outputs)} localized IWDs, "
          f"{total / (1024 * 1024):.1f} MB packed on disk -> {out_main}", flush=True)
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("install_root", help="CoD2 install root (contains main/)")
    ap.add_argument("-o", "--output", required=True, help="Output pack root (will contain main/)")
    ap.add_argument("--base-packs", type=int, default=DEFAULT_BASE_PACKS,
                    help=f"Number of iw_XX.iwd files (default {DEFAULT_BASE_PACKS})")
    ap.add_argument("--loc-packs", type=int, default=DEFAULT_LOC_PACKS,
                    help=f"Number of localized_english_iwXX.iwd (default {DEFAULT_LOC_PACKS})")
    args = ap.parse_args()
    return build(
        os.path.abspath(args.install_root),
        os.path.abspath(args.output),
        args.base_packs,
        args.loc_packs,
    )


if __name__ == "__main__":
    sys.exit(main())
