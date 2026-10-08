#!/usr/bin/env python3
# -*- coding: utf-8 -*-
#
# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
"""Copy the missing CLK_HROW BUFG-cascade rows from artix7 into a sibling
7-series family database.

CLK_HROW_BOT_R and CLK_HROW_TOP_R are the same tile type on every 7-series
family, and the rows both families already resolved are bit-identical
(measured: every one of the 2804 shared bot_r rows and every shared top_r
row, zynq7 and spartan7 against artix7). What is missing on the sibling
side is a subset of the CK_BUFG_CASCO rows that artix7's 041-clk-hrow-pips
population resolved and the sibling's did not:

  * the 32 pass-through rows `CK_BUFG_CASCO<i> <- CK_BUFG_CASCIN<i>`
    (zynq7 and spartan7 bot_r): the vertical hop a pad clock takes through
    each CLK_HROW tile between its entry row and the central BUFGs.
  * the 448 fan-in rows `CK_BUFG_CASCO<i> <- CK_BUFRCLK_L*/CK_IN_L*`
    (zynq7 top_r): the entry of a left-side row clock onto the cascade.

This tool copies exactly those rows, and refuses to run if the premise is
not measurable in the target database:

  1. every row key shared with artix7 must be bit-identical (whole file,
     not just the CASC rows);
  2. the target may not have any CASC row artix7 lacks;
  3. no copied bit pattern may collide with (equal) an existing row's
     pattern, or another copied row's;
  4. every copied row's pip must exist in the target family's
     tile_type_CLK_HROW_*.json (the hole is in the segbits, never in the
     tile structure).

Without --apply it only reports. With --apply it appends the rows (with
`origin:041a-clk-hrow-casc-copy` in the .origin_info.db) and re-canonicalizes
the touched files with utils/sort_db.py so no pre-existing line moves.
"""
import argparse
import json
import os
import subprocess
import sys
from pathlib import Path

ORIGIN = "041a-clk-hrow-casc-copy"
TILES = ("clk_hrow_bot_r", "clk_hrow_top_r")
SOURCE_FAMILY = "artix7"


def load_segbits(path):
    rows = {}
    for line in path.read_text().splitlines():
        if not line.strip():
            continue
        feature, _, bits = line.partition(" ")
        rows[feature] = bits
    return rows


def bit_pattern(bits):
    return frozenset(bits.split())


def tile_pips(db_root, family, tile):
    tile_json = db_root / family / f"tile_type_{tile.upper()}.json"
    with tile_json.open() as f:
        return set(json.load(f)["pips"].keys())


def missing_casc_rows(db_root, family, tile, errors):
    src = load_segbits(db_root / SOURCE_FAMILY / f"segbits_{tile}.db")
    dst = load_segbits(db_root / family / f"segbits_{tile}.db")

    shared = set(src) & set(dst)
    differing = sorted(k for k in shared if src[k] != dst[k])
    if differing:
        errors.append(
            f"{family}/{tile}: {len(differing)} shared rows differ from "
            f"{SOURCE_FAMILY} (first: {differing[0]}); the copy premise "
            f"does not hold")

    extra = sorted(k for k in dst if "CASC" in k and k not in src)
    if extra:
        errors.append(
            f"{family}/{tile}: {len(extra)} CASC rows not present in "
            f"{SOURCE_FAMILY} (first: {extra[0]})")

    candidates = {
        k: src[k]
        for k in sorted(src)
        if "CASC" in k and k not in dst
    }

    dst_patterns = {bit_pattern(v): k for k, v in dst.items()}
    seen = {}
    for k, v in candidates.items():
        pattern = bit_pattern(v)
        if pattern in dst_patterns:
            errors.append(
                f"{family}/{tile}: {k} has the same bit pattern as the "
                f"existing row {dst_patterns[pattern]}")
        if pattern in seen:
            errors.append(
                f"{family}/{tile}: {k} has the same bit pattern as the "
                f"copied row {seen[pattern]}")
        seen[pattern] = k

    pips = tile_pips(db_root, family, tile)
    for k in candidates:
        tile_name, dst_wire, src_wire = k.split(".")
        pip = f"{tile_name}.{src_wire}->>{dst_wire}"
        if pip not in pips:
            errors.append(
                f"{family}/{tile}: pip {pip} is not in the {family} "
                f"tile_type_{tile.upper()}.json")

    return candidates


def classify(feature):
    return "pass-through" if "CASCIN" in feature else "fan-in"


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("family", choices=("zynq7", "spartan7"))
    parser.add_argument(
        "--db-root",
        type=Path,
        required=True,
        help="prjxray-db checkout to read artix7 from and patch")
    parser.add_argument(
        "--apply",
        action="store_true",
        help="append the rows and re-canonicalize (default: report only)")
    args = parser.parse_args()

    errors = []
    per_tile = {
        tile: missing_casc_rows(args.db_root, args.family, tile, errors)
        for tile in TILES
    }
    if errors:
        for e in errors:
            print(f"ERROR: {e}", file=sys.stderr)
        return 1

    total = 0
    for tile, rows in per_tile.items():
        counts = {}
        for k in rows:
            counts[classify(k)] = counts.get(classify(k), 0) + 1
        detail = ", ".join(f"{n} {c}" for c, n in sorted(counts.items()))
        print(
            f"{args.family}/{tile}: {len(rows)} rows to copy" +
            (f" ({detail})" if detail else " (complete)"))
        for k, v in rows.items():
            print(f"  {k} {v}")
        total += len(rows)

    if not args.apply:
        print(f"{total} rows total (report only, nothing written)")
        return 0

    prjxray_root = Path(__file__).resolve().parent.parent.parent
    sort_db = prjxray_root / "utils" / "sort_db.py"
    touched = []
    for tile, rows in per_tile.items():
        if not rows:
            continue
        segbits = args.db_root / args.family / f"segbits_{tile}.db"
        origin = args.db_root / args.family / \
            f"segbits_{tile}.origin_info.db"
        with segbits.open("a") as f:
            for k, v in rows.items():
                f.write(f"{k} {v}\n")
        with origin.open("a") as f:
            for k, v in rows.items():
                f.write(f"{k} origin:{ORIGIN} {v}\n")
        touched += [segbits, origin]
    env = dict(os.environ, PYTHONPATH=str(prjxray_root))
    for path in touched:
        subprocess.check_call(
            [sys.executable, str(sort_db),
             str(path.resolve())],
            cwd=prjxray_root,
            env=env)
    print(
        f"{total} rows appended and canonicalized in "
        f"{len(touched)} files")
    return 0


if __name__ == "__main__":
    sys.exit(main())
