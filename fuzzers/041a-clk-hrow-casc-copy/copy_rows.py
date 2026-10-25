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
"""Copy the missing CLK_HROW rows from artix7 into a sibling 7-series
family database.

CLK_HROW_BOT_R and CLK_HROW_TOP_R are the same tile type on every 7-series
family, and the rows both families already resolved are bit-identical
(measured: every shared row of the two segbits files, zynq7 and spartan7
against artix7). What is missing on the sibling side is a subset of the rows
that artix7's 041-clk-hrow-pips population resolved and the sibling's did
not:

  * the 32 pass-through rows `CK_BUFG_CASCO<i> <- CK_BUFG_CASCIN<i>`
    (zynq7 and spartan7 bot_r): the vertical hop a pad clock takes through
    each CLK_HROW tile between its entry row and the central BUFGs.
  * the 448 fan-in rows `CK_BUFG_CASCO<i> <- CK_BUFRCLK_L*/CK_IN_L*`
    (zynq7 top_r): the entry of a left-side row clock onto the cascade.
  * the 240 BUFH rows `CK_MUX_OUT_L<i>/CK_MUX_OUT_R<i> <- CK_IN_L4..13`
    (zynq7 top_r): the same left-side row clocks into the BUFHs.
  * the 14 `CLK_HROW_CK_IN_L4..13_ACTIVE` / `CLK_HROW_CK_BUFRCLK_L0..3_ACTIVE`
    rows (zynq7 top_r): the enable of each of those left-side wires, set
    whenever the wire carries a clock.

This tool copies exactly those rows, and refuses to run if the premise is
not measurable in the target database:

  1. every row key shared with artix7 must be bit-identical (whole file,
     not just the copied classes);
  2. the target may not have any row artix7 lacks;
  3. no copied bit pattern may collide with (equal) an existing row's
     pattern, or another copied row's;
  4. every copied pip row's pip must exist in the target family's
     tile_type_CLK_HROW_*.json, and every copied `<wire>_ACTIVE` row's wire
     must exist there too (the hole is in the segbits, never in the tile
     structure);
  5. every bit the rows use must be in the target's mask_clk_hrow_*.db;
     the bits missing there may only be bits artix7's mask has, and the
     target's mask may not have a bit artix7's lacks.

Without --apply it only reports. With --apply it holds the family directory
locked for the whole run and works out each file on its own: the rows the
segbits lack, an `origin:041a-clk-hrow-casc-copy` line in the
.origin_info.db for every row that has none (every other row has one), and
the mask bits. Then it re-canonicalizes the six files with
utils/sort_db.py, so no pre-existing line moves. A run that stopped halfway
is completed by the next one.
"""
import argparse
import fcntl
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


def load_mask(path):
    return {
        line.split()[1]
        for line in path.read_text().splitlines()
        if line.strip()
    }


def tile_type_json(db_root, family, tile):
    tile_json = db_root / family / f"tile_type_{tile.upper()}.json"
    with tile_json.open() as f:
        return json.load(f)


def missing_rows(db_root, family, tile, errors):
    src = load_segbits(db_root / SOURCE_FAMILY / f"segbits_{tile}.db")
    dst = load_segbits(db_root / family / f"segbits_{tile}.db")

    shared = set(src) & set(dst)
    differing = sorted(k for k in shared if src[k] != dst[k])
    if differing:
        errors.append(
            f"{family}/{tile}: {len(differing)} shared rows differ from "
            f"{SOURCE_FAMILY} (first: {differing[0]}); the copy premise "
            f"does not hold")

    extra = sorted(k for k in dst if k not in src)
    if extra:
        errors.append(
            f"{family}/{tile}: {len(extra)} rows not present in "
            f"{SOURCE_FAMILY} (first: {extra[0]})")

    candidates = {k: src[k] for k in sorted(src) if k not in dst}

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

    tile_type = tile_type_json(db_root, family, tile)
    for k in candidates:
        parts = k.split(".")
        if len(parts) == 3:
            tile_name, dst_wire, src_wire = parts
            pip = f"{tile_name}.{src_wire}->>{dst_wire}"
            if pip not in tile_type["pips"]:
                errors.append(
                    f"{family}/{tile}: pip {pip} is not in the {family} "
                    f"tile_type_{tile.upper()}.json")
        else:
            wire = parts[1][:-len("_ACTIVE")]
            if not parts[1].endswith("_ACTIVE") or \
                    wire not in tile_type["wires"]:
                errors.append(
                    f"{family}/{tile}: {k} is not the _ACTIVE row of a "
                    f"wire of the {family} tile_type_{tile.upper()}.json")

    return candidates


def missing_origins(db_root, family, tile, rows):
    segbits = load_segbits(db_root / family / f"segbits_{tile}.db")
    segbits.update(rows)
    origin = db_root / family / f"segbits_{tile}.origin_info.db"
    known = {
        line.split()[0]
        for line in origin.read_text().splitlines()
        if line.strip()
    }
    return {k: v for k, v in sorted(segbits.items()) if k not in known}


def missing_mask_bits(db_root, family, tile, rows, errors):
    src = load_mask(db_root / SOURCE_FAMILY / f"mask_{tile}.db")
    dst = load_mask(db_root / family / f"mask_{tile}.db")
    extra = sorted(dst - src)
    if extra:
        errors.append(
            f"{family}/{tile}: {len(extra)} mask bits not present in "
            f"{SOURCE_FAMILY} (first: {extra[0]})")
    segbits = load_segbits(db_root / family / f"segbits_{tile}.db")
    segbits.update(rows)
    used = {b.lstrip("!") for v in segbits.values() for b in v.split()}
    bits = sorted(used - dst)
    outside = [b for b in bits if b not in src]
    if outside:
        errors.append(
            f"{family}/{tile}: {len(outside)} bits used by a row are in "
            f"neither mask (first: {outside[0]})")
    return bits


def classify(feature):
    parts = feature.split(".")
    if len(parts) == 2:
        return "_ACTIVE"
    if "MUX_OUT" in parts[1]:
        return "BUFH"
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

    if args.apply:
        # Two runs at once would both append the same lines: the second one
        # waits here and then reads what the first one wrote.
        fcntl.flock(
            os.open(args.db_root / args.family, os.O_RDONLY), fcntl.LOCK_EX)

    errors = []
    per_tile = {
        tile: missing_rows(args.db_root, args.family, tile, errors)
        for tile in TILES
    }
    origins = {
        tile: missing_origins(args.db_root, args.family, tile, per_tile[tile])
        for tile in TILES
    }
    mask_bits = {
        tile: missing_mask_bits(
            args.db_root, args.family, tile, per_tile[tile], errors)
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
            (f" ({detail})" if detail else " (complete)") +
            f", {len(mask_bits[tile])} mask bits to add")
        for k, v in rows.items():
            print(f"  {k} {v}")
        for k in origins[tile]:
            if k not in rows:
                print(f"  origin line for {k}")
        for b in mask_bits[tile]:
            print(f"  mask bit {b}")
        total += len(rows)

    if not args.apply:
        print(f"{total} rows total (report only, nothing written)")
        return 0

    prjxray_root = Path(__file__).resolve().parent.parent.parent
    sort_db = prjxray_root / "utils" / "sort_db.py"
    touched = []
    for tile in TILES:
        segbits = args.db_root / args.family / f"segbits_{tile}.db"
        origin = args.db_root / args.family / f"segbits_{tile}.origin_info.db"
        mask = args.db_root / args.family / f"mask_{tile}.db"
        if per_tile[tile]:
            with segbits.open("a") as f:
                for k, v in per_tile[tile].items():
                    f.write(f"{k} {v}\n")
        if origins[tile]:
            with origin.open("a") as f:
                for k, v in origins[tile].items():
                    f.write(f"{k} origin:{ORIGIN} {v}\n")
        if mask_bits[tile]:
            with mask.open("a") as f:
                for b in mask_bits[tile]:
                    f.write(f"bit {b}\n")
        # All three, even with nothing added: a run that stopped before
        # sorting left lines appended at the end.
        touched += [segbits, origin, mask]
    env = dict(os.environ, PYTHONPATH=str(prjxray_root))
    for path in touched:
        subprocess.check_call(
            [sys.executable, str(sort_db),
             str(path.resolve())],
            cwd=prjxray_root,
            env=env)
    print(f"{total} rows appended, {len(touched)} files canonicalized")
    return 0


if __name__ == "__main__":
    sys.exit(main())
