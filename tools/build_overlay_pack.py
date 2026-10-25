#!/usr/bin/env python3
"""Build a replacement ("overlay") IWD that overrides selected game files.

CoD2 searches IWD archives in reverse alphabetical order, so a file inside
main/zz_overlay_<name>.iwd takes precedence over the same path in the
original archives. This is the way to swap out individual assets (a texture,
a sound, a model, a menu) for legally usable equivalents without touching
the engine, the other archives or the architecture.

  python tools/build_overlay_pack.py replacements/ --name licensed \\
      -o build/web/main/zz_overlay_licensed.iwd --base build/web/main

`replacements/` mirrors game paths, e.g. replacements/sound/weapons/x.wav.
With --base, the report lists which original archive each overridden path
came from and which replacement paths do not exist in the base game (typos).

The server must load the same overlay when sv_pure is enabled. Do not put
original game files into an overlay you distribute.
"""

from __future__ import annotations

import argparse
import json
import os
import re
import sys
import zipfile

_NAME_RE = re.compile(r"^[a-z0-9_]{1,32}$")
_STORE_EXT = (".iwi", ".jpg", ".png", ".mp3", ".ogg", ".roq", ".bik")


def collect(src: str) -> list[tuple[str, str]]:
    out = []
    root = os.path.abspath(src)
    for dirpath, _dirs, files in os.walk(root):
        for name in files:
            full = os.path.join(dirpath, name)
            rel = os.path.relpath(full, root).replace("\\", "/")
            if rel.startswith("../") or rel.startswith("/") or ".." in rel.split("/"):
                raise SystemExit("unsafe path %r" % rel)
            if rel.lower().endswith((".iwd", ".pk3", ".zip")):
                raise SystemExit("nested archive not allowed: %s" % rel)
            out.append((rel, full))
    out.sort()
    return out


def base_index(base_dir: str, skip: str | None = None) -> dict[str, str]:
    """Lower-case path -> archive that provides it last (highest priority).

    ``skip`` excludes the overlay being built when it lives in base_dir."""
    index: dict[str, str] = {}
    skip_name = os.path.basename(skip).lower() if skip else None
    names = sorted(n for n in os.listdir(base_dir)
                   if n.lower().endswith(".iwd") and n.lower() != skip_name)
    for name in names:   # alphabetical; later archives win
        with zipfile.ZipFile(os.path.join(base_dir, name)) as z:
            for info in z.infolist():
                if not info.is_dir():
                    index[info.filename.replace("\\", "/").lower()] = name
    return index


def build(src: str, out: str, base: str | None = None) -> dict:
    files = collect(src)
    if not files:
        raise SystemExit("no files under %s" % src)
    # Index the base before writing, without the overlay itself.
    same_dir = base and os.path.abspath(os.path.dirname(os.path.abspath(out))) == os.path.abspath(base)
    index = base_index(base, out if same_dir else None) if base else None
    os.makedirs(os.path.dirname(os.path.abspath(out)) or ".", exist_ok=True)
    tmp = out + ".tmp"
    with zipfile.ZipFile(tmp, "w") as z:
        for rel, full in files:
            method = zipfile.ZIP_STORED if rel.lower().endswith(_STORE_EXT) else zipfile.ZIP_DEFLATED
            z.write(full, rel, compress_type=method)
    os.replace(tmp, out)
    report = {"overlay": os.path.basename(out), "files": len(files), "overrides": {}, "new": []}
    if index is not None:
        for rel, _ in files:
            src_archive = index.get(rel.lower())
            if src_archive:
                report["overrides"][rel] = src_archive
            else:
                report["new"].append(rel)
    return report


def main(argv=None) -> int:
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("source", help="folder mirroring game paths")
    ap.add_argument("--name", required=True, help="overlay name: [a-z0-9_]")
    ap.add_argument("-o", "--output", help="output .iwd (default: zz_overlay_<name>.iwd here)")
    ap.add_argument("--base", help="folder with the base main/*.iwd for the override report")
    args = ap.parse_args(argv)
    if not _NAME_RE.match(args.name):
        ap.error("--name must match [a-z0-9_]{1,32}")
    out = args.output or "zz_overlay_%s.iwd" % args.name
    if not os.path.basename(out).lower().startswith("zz_"):
        ap.error("the output name must start with zz_ so it overrides the base archives")
    report = build(args.source, out, args.base)
    json.dump(report, sys.stdout, indent=2)
    sys.stdout.write("\n")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
