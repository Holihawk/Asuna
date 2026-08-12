#!/usr/bin/env python3
"""Fetch Live2D outfit assets referenced by a weblive2d model index.

The widget the model was captured from serves every outfit from the same npm
mirror; each index.json names its own .moc, textures, expressions, motions and
physics, so the whole set is derivable from the index alone.

    tools/fetch_models.py --list
    tools/fetch_models.py 02 04 07
    tools/fetch_models.py --all

Assets are third-party copyrighted artwork. This downloads them for local use
only - see the README under Licensing before publishing anything that contains models/.
"""

import argparse
import json
import os
import sys
import urllib.error
import urllib.request
from concurrent.futures import ThreadPoolExecutor

BASE = "https://registry.npmmirror.com/weblive2d/latest/files"
MODELS = BASE + "/model/asuna"
DEST = os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "models")

# model_list.json order; the gaps (10, 11, 32, 42) are absent upstream.
IDS = ["01", "02", "03", "04", "05", "06", "07", "08", "09", "12", "13", "14",
       "15", "16", "17", "18", "19", "20", "21", "22", "23", "24", "25", "26",
       "27", "28", "29", "30", "31", "33", "34", "35", "36", "37", "38", "39",
       "40", "41", "43", "44", "45", "46"]


def get(url, timeout=40):
    req = urllib.request.Request(url, headers={"User-Agent": "asuna-desktop-pet/0.1"})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return r.read()


def asset_paths(index):
    """Every file an index.json depends on, as paths relative to the model dir."""
    paths = [index["model"]]
    paths += index.get("textures", [])
    paths += [e["file"] for e in index.get("expressions", [])]
    for group in index.get("motions", {}).values():
        paths += [m["file"] for m in group if "file" in m]
        paths += [m["sound"] for m in group if "sound" in m]
    for key in ("physics", "pose"):
        if index.get(key):
            paths.append(index[key])
    # Dedup while keeping order, so the log reads in dependency order.
    seen, out = set(), []
    for p in paths:
        if p not in seen:
            seen.add(p)
            out.append(p)
    return out


def fetch_model(mid, force=False):
    out_dir = os.path.join(DEST, f"asuna_{mid}")
    index_path = os.path.join(out_dir, "index.json")
    os.makedirs(out_dir, exist_ok=True)

    if force or not os.path.exists(index_path):
        data = get(f"{MODELS}/asuna_{mid}/index.json")
        with open(index_path, "wb") as f:
            f.write(data)
    index = json.load(open(index_path))

    todo = []
    for rel in asset_paths(index):
        dst = os.path.join(out_dir, rel)
        if force or not os.path.exists(dst) or os.path.getsize(dst) == 0:
            todo.append(rel)

    if not todo:
        return mid, len(asset_paths(index)), 0, None

    def one(rel):
        dst = os.path.join(out_dir, rel)
        os.makedirs(os.path.dirname(dst), exist_ok=True)
        data = get(f"{MODELS}/asuna_{mid}/{rel}")
        with open(dst, "wb") as f:
            f.write(data)

    errors = []
    with ThreadPoolExecutor(max_workers=4) as pool:
        for rel, res in zip(todo, pool.map(lambda r: _safe(one, r), todo)):
            if res is not None:
                errors.append(f"{rel}: {res}")
    return mid, len(asset_paths(index)), len(todo), errors or None


def _safe(fn, arg):
    try:
        fn(arg)
        return None
    except (urllib.error.URLError, OSError) as e:
        return str(e)


def main():
    ap = argparse.ArgumentParser(description=__doc__,
                                 formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("ids", nargs="*", help="outfit ids, e.g. 02 04")
    ap.add_argument("--all", action="store_true", help="fetch every known outfit")
    ap.add_argument("--list", action="store_true", help="print known outfit ids")
    ap.add_argument("--force", action="store_true", help="re-download existing files")
    args = ap.parse_args()

    if args.list:
        print(" ".join(IDS))
        return 0

    ids = IDS if args.all else args.ids
    if not ids:
        ap.print_usage()
        return 2

    failed = False
    for mid in ids:
        try:
            mid, total, got, errors = fetch_model(mid, args.force)
        except (urllib.error.URLError, OSError, ValueError) as e:
            print(f"asuna_{mid}: FAILED ({e})", file=sys.stderr)
            failed = True
            continue
        status = f"{got} new / {total} files"
        if errors:
            failed = True
            status += f"  ERRORS: {'; '.join(errors[:3])}"
        print(f"asuna_{mid}: {status}")
    return 1 if failed else 0


if __name__ == "__main__":
    sys.exit(main())
