#!/usr/bin/env python3
"""Bit-level verification of the copied CLK_HROW CASC rows against a Vivado
specimen bitstream.

Inputs: the specimen's routed CLK_HROW pips (hrow_pips.txt, dumped by
run.tcl from the routed design), the specimen decoded with bit2fasm
--verbose against the UNPATCHED db (before.fasm) and against the PATCHED
db (after.fasm), the patched family db and the part's tilegrid.

Checks:
  1. every routed CASC pip that corresponds to a COPIED row is absent as a
     feature in before.fasm and present in after.fasm;
  2. the expected (frame, word, bit) positions of those rows -- computed
     from tilegrid + the copied bit patterns -- appear as unknown bits in
     before.fasm and NOT in after.fasm;
  3. the unknown bits that disappear between before and after are EXACTLY
     the expected positions (nothing else changed);
  4. routed CASC pips whose rows pre-exist decode as features in BOTH
     (calibration: the tile/frame math and the db agree on known rows).
"""
import json
import re
import sys
from pathlib import Path

spec_dir = Path(sys.argv[1])  # holds hrow_pips.txt, before.fasm, after.fasm
db_patched = Path(sys.argv[2])  # patched family dir (e.g. db-patched/zynq7)
tilegrid_p = Path(sys.argv[3])  # tilegrid.json of the base part
copied_p = Path(sys.argv[4])  # file listing the copied rows "FEATURE BITS"

# --- the copied rows -------------------------------------------------------
copied = {}
for line in copied_p.read_text().splitlines():
    line = line.strip()
    if not line or line.startswith("#"):
        continue
    feat, _, bits = line.partition(" ")
    copied[feat] = bits.split()

# --- the family segbits (patched) -----------------------------------------
segbits = {}
for tile_type in ("clk_hrow_bot_r", "clk_hrow_top_r"):
    p = db_patched / f"segbits_{tile_type}.db"
    for line in p.read_text().splitlines():
        if line.strip():
            feat, _, bits = line.partition(" ")
            segbits[feat] = bits.split()

# --- the routed CASC pips of the specimen ----------------------------------
# hrow_pips.txt lines: "<net> <TILE>/<TILE_TYPE>.<SRC>->><DST>" (also <<->>)
used = set()  # (tile_name, segbits_key)
for line in (spec_dir / "hrow_pips.txt").read_text().splitlines():
    m = re.search(r"(\S+)/([A-Z0-9_]+)\.(\S+?)(<<)?->>?(\S+)$", line.strip())
    if not m:
        continue
    tile_name, tile_type, src, _, dst = m.groups()
    if "CASC" not in src and "CASC" not in dst:
        continue
    used.add((tile_name, f"{tile_type}.{dst}.{src}"))

if not used:
    print("FAIL: the specimen routed no CASC pip at all")
    sys.exit(1)

# --- expected bit positions ------------------------------------------------
grid = json.load(tilegrid_p.open())


def expected_positions(tile_name, bits):
    info = grid[tile_name]["bits"]["CLB_IO_CLK"]
    base = int(info["baseaddr"], 16)
    out = set()
    for b in bits:
        assert not b.startswith("!"), (tile_name, b)
        fo, bo = (int(x) for x in b.split("_"))
        frame = base + fo
        bit = info["offset"] * 32 + bo
        out.add("{:08x}_{}_{}".format(frame, bit // 32, bit % 32))
    return out


exp_copied = {}  # positions expected to move from unknown -> decoded
calib = []  # (tile, key) with pre-existing rows
for tile_name, key in sorted(used):
    if key in copied:
        exp_copied[(tile_name,
                    key)] = expected_positions(tile_name, copied[key])
    elif key in segbits:
        calib.append((tile_name, key))
    else:
        print(
            f"WARN: routed CASC pip has no row even in the patched db: "
            f"{tile_name} {key}")


# --- parse the two fasm outputs --------------------------------------------
def parse(path):
    feats = set()
    unknown = set()
    for line in path.read_text().splitlines():
        m = re.search(r'unknown_bit\s*=\s*"([0-9a-f]+_\d+_\d+)"', line)
        if m:
            unknown.add(m.group(1))
            continue
        line = line.split("#")[0].strip()
        if line and not line.startswith("{"):
            feats.add(line.split("=")[0].strip().split(" ")[0])
    return feats, unknown


before_f, before_u = parse(spec_dir / "before.fasm")
after_f, after_u = parse(spec_dir / "after.fasm")

fails = 0

# check 4 (calibration) first
for tile_name, key in calib:
    fasm_feature = f"{tile_name}.{key.split('.', 1)[1]}"
    ok_b = fasm_feature in before_f
    ok_a = fasm_feature in after_f
    print(
        f"CALIB {'ok' if ok_b and ok_a else 'FAIL'}: {fasm_feature} "
        f"(before={ok_b} after={ok_a})")
    if not (ok_b and ok_a):
        fails += 1

# checks 1-2
all_expected = set()
for (tile_name, key), positions in sorted(exp_copied.items()):
    all_expected |= positions
    fasm_feature = f"{tile_name}.{key.split('.', 1)[1]}"
    in_b = fasm_feature in before_f
    in_a = fasm_feature in after_f
    pos_b = positions <= before_u
    pos_a = positions & after_u
    ok = (not in_b) and in_a and pos_b and not pos_a
    print(f"COPIED {'ok' if ok else 'FAIL'}: {fasm_feature}")
    print(f"    feature: before={in_b} (want False)  after={in_a} (want True)")
    print(
        f"    bits {sorted(positions)}: unknown-before={pos_b} (want True)  "
        f"unknown-after={sorted(pos_a)} (want empty)")
    if not ok:
        fails += 1

# check 3: the unknowns that disappeared are exactly the expected ones
disappeared = before_u - after_u
if disappeared == all_expected:
    print(
        f"DIFF ok: unknown bits resolved by the patch == expected "
        f"positions ({len(all_expected)})")
else:
    print(
        f"DIFF FAIL: resolved-not-expected={sorted(disappeared - all_expected)} "
        f"expected-not-resolved={sorted(all_expected - disappeared)}")
    fails += 1

print(
    f"copied-rows-exercised={len(exp_copied)} calibration-rows={len(calib)} "
    f"unknown-before={len(before_u)} unknown-after={len(after_u)}")
print("VERIFY", "FAIL" if fails else "PASS")
sys.exit(1 if fails else 0)
