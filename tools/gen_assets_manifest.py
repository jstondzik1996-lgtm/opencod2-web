#!/usr/bin/env python3
"""Generate assets.json for the web remote loader from a CoD2 install tree.

Usage:
  python3 tools/gen_assets_manifest.py /home/cod2server -o /var/www/cod2-web/assets.json

Scans main/ (and optionally zone/) for engine-needed extensions and writes a
manifest the browser fetch()es, then caches into OPFS.
"""
from __future__ import annotations

import argparse
import json
import os
import sys

WANTED_EXT = (
    ".iwd", ".cfg", ".dat", ".txt", ".menu", ".so", ".dll",
    ".bik", ".roq", ".str", ".gsc", ".csc", ".arena",
)
WANTED_DIRS = ("main", "zone")


def wanted(name: str) -> bool:
    lower = name.lower()
    return any(lower.endswith(ext) for ext in WANTED_EXT)


def scan(root: str) -> list[dict]:
    files: list[dict] = []
    for top in WANTED_DIRS:
        base = os.path.join(root, top)
        if not os.path.isdir(base):
            continue
        for dirpath, _dirnames, filenames in os.walk(base):
            for name in filenames:
                if not wanted(name):
                    continue
                full = os.path.join(dirpath, name)
                rel = os.path.relpath(full, root).replace("\\", "/")
                st = os.stat(full)
                # Cache key for the browser: a replaced archive with the same
                # size still gets downloaded again.
                files.append({"path": rel, "size": st.st_size,
                              "version": "%x-%x" % (st.st_size, int(st.st_mtime))})
    files.sort(key=lambda r: r["path"])
    return files


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("install_root", help="CoD2 root containing main/")
    ap.add_argument("-o", "--output", default="-", help="Output path or - for stdout")
    ap.add_argument("--base-url", default="/", help="HTTP base URL for asset paths")
    args = ap.parse_args()

    root = os.path.abspath(args.install_root)
    if not os.path.isdir(os.path.join(root, "main")):
        print(f"error: {root}/main not found", file=sys.stderr)
        return 1

    files = scan(root)
    iwd = [f for f in files if f["path"].lower().startswith("main/") and f["path"].lower().endswith(".iwd")]
    if not iwd:
        print("error: no main/*.iwd found", file=sys.stderr)
        return 1

    doc = {
        "baseUrl": args.base_url,
        "files": files,
        "fileCount": len(files),
        "bytesTotal": sum(f["size"] for f in files),
    }
    text = json.dumps(doc, indent=2) + "\n"
    if args.output == "-":
        sys.stdout.write(text)
    else:
        out = os.path.abspath(args.output)
        os.makedirs(os.path.dirname(out) or ".", exist_ok=True)
        with open(out, "w", encoding="utf-8") as fh:
            fh.write(text)
        print(f"wrote {out} ({len(files)} files, {doc['bytesTotal'] / (1024*1024):.1f} MB)", file=sys.stderr)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
