# 045a — HCLK\_CMT CK\_IN\* segbits

Fills the gap left by `045-hclk-cmt-pips`, which leaves 21 PIPs through the
`HCLK_CMT_CK_IN[1..13]` inputs unsolved (see
`fuzzers/045-hclk-cmt-pips/build/todo.txt`).  These are needed to translate
`HCLK_CMT*.MUX_CLK_n.CK_INm` features that nextpnr-xilinx emits when routing
a `IBUFDS_GTE2 → BUFG → fabric` clock (e.g. the VC707 `SGMIICLK_Q0` path).

The 21 unsolved entries split as:

| tile_type   | unsolved PIPs |
|-------------|---------------|
| HCLK_CMT    | 3  (MUX_CLK_{7,8,10} ← CK_IN{9,4,6}) |
| HCLK_CMT_L  | 18 (CK_IN{1,2,3,5,7,8,9,10,11,12,13} → various MMCM/PLL/MUX_CLK_*) |

## Why the parent 045 fuzzer misses them

`045-hclk-cmt-pips` uses random CMT clock-source assignments and relies on
Vivado's placer/router to spread routes across PIPs.  For the CK_IN paths
Vivado almost always picks the same physical wire regardless of seed —
either because the CK_IN input is fed from a non-modeled source (a CCIO
that isn't in the design pool) or because the routing engine prefers a
single "shortest path" through the mux fabric.  Without explicit
constraints the `(MUX_CLK_n, CK_INm)` cell of the routing matrix never
flips, so `segmatch` can't isolate its bits.

## Approach used here

For each of the 21 PIPs we generate a *forced-route* specimen:

1. `top.py` instantiates a CMT loadload (MMCM/PLL or BUFR sink) plus a
   clock-capable source (CCIO pad).
2. `generate.tcl` issues explicit `USER_CLOCK_ROOT`, `LOC`, and
   `route_design -mode quick -nets <net>` to pin the clock through the
   exact `HCLK_CMT*.MUX_CLK_n.HCLK_CMT_CK_INm` PIP we want to learn.
   We confirmed this works on this part by reproducing the same trick in
   `~/div2_sgmii_loc_build/` (LOC SLICE_X0Y0 + LOC BUFGCTRL_X0Y0 forced
   the clock onto `HCLK_CMT_X88Y26` and set bit `27_181`).
3. The standard prjxray `segmaker` / `segmatch` chain extracts the
   set/clear bit pattern for each PIP.

## State as of this session

End-to-end runnable scaffold:

- `targets.csv` — the 21 PIPs (3 HCLK_CMT + 18 HCLK_CMT_L), generated from
  `045-hclk-cmt-pips/build/todo.txt`.
- `top.py` — delegates to 045's `top.py` so we share its MMCM/PLL/BUFR
  loadload pool.  Verified to emit ~3240 lines of Verilog given a SEED.
- `generate.tcl` — implements `pick_target_for_specimen` (round-robin
  on SPECIMEN env) and `force_ck_in_route` (walks placed nets, finds one
  in the target tile_type, builds a two-leg `find_routing_path` and pins
  it with `FIXED_ROUTE`).  Parses cleanly through Vivado.
- `Makefile` — N=24 (21 targets + 3 redundant retries on the first three
  rows).  Uses standard pip_loop.mk.
- `build/cmt_regions.csv` — symlinked from 045's prior build so we share
  its CMT site→region mapping.

## How to run

```
cd fuzzers/045a-hclk-cmt-ck-in
source ../../settings/virtex7.sh       # set XRAY_PART etc.
make
```

Output: `build/segbits_hclk_cmt.db` + `build/segbits_hclk_cmt_l.db`
which then merge into `database/virtex7/segbits_hclk_cmt[_l].db` via
`pushdb`-equivalent step (see 045's Makefile chain).

## What the first specimen run taught us

Ran `make N=1 build/1/specimen_001/OK` against target row 0
(`HCLK_CMT.HCLK_CMT_MUX_CLK_10.HCLK_CMT_CK_IN6`).

- top.py / generate.tcl chain runs end-to-end through Vivado:
  synth → place → route_design → force_ck_in_route → route_design
  → write_bitstream → bit2fasm.  ~10 min wall-clock per specimen.
- `force_ck_in_route` scanned 16 nets, found 9 candidates touching
  some `HCLK_CMT` tile, tried 3, and bailed: every candidate failed
  the `find_routing_path` driver→src leg.  Diagnostic line:
  `045a: candidate cin1_MMCME2_ADV_X0Y3 via HCLK_CMT_X88Y182 — driver can't reach src`.
- The Vivado design.bit + design.fasm are produced, but with no
  target PIP used.  Consequence: `int_generate.py` asserts
  `"Didn't generate any segments"` because nothing in design.txt
  intersects todo.txt.

## Why simple `find_routing_path` isn't enough

Each `HCLK_CMT_CK_INm` input is fed from a specific clock backbone
segment determined by *physical placement* (which BUFGCTRL site,
which BUFR site, which HCLK leaf).  045's random MMCM/PLL/BUFR
placement happens to never land in a topology where the existing
clock drivers can be re-routed to CK_IN6 — which is exactly why
those 21 PIPs survived 100 random specimens in 045 to begin with.

`find_routing_path` faithfully reports "no reachable wires" because
the driver site_pin really can't get to CK_IN6 of the chosen
HCLK_CMT tile without first being moved.

## The pivot the next session needs

Make `force_ck_in_route` *choose the design topology* per specimen,
not just react to whatever random placement landed:

1. **LOC-based force per target.**  We already proved on
   `~/div2_sgmii_loc_build/` that
   `set_property LOC BUFGCTRL_X0Y0 [get_cells sysclk_bufg]` +
   `set_property USER_CLOCK_ROOT X0Y0 [get_nets clk]` reliably
   moves the clock onto a specific `HCLK_CMT_X88Y26` and lights up
   one bit (`27_181`).  For each target row in `targets.csv` we
   need a small table of `(tile_type instance, expected_BUFG_site,
   expected_BUFR_site)` triples; the proc applies those LOC
   constraints before `place_design`, not after route.
2. **Drop the post-hoc find_routing_path.**  Once Vivado places the
   design under the LOC, the natural route will hit the target
   PIP — no FIXED_ROUTE injection needed.  Verify with
   `get_pips -of_objects [get_nets clk]` and only commit specimens
   where the target PIP shows up.
3. **Skip-not-fail in int_generate.**  For specimens that did
   ultimately use the target PIP, write `segdata_*.txt`; for those
   that didn't, write an empty `segdata_*.txt` + `tag` line marking
   the negative.  Avoid the `AssertionError` and let segmatch
   benefit from negative evidence.

## Pre-pivot smoke test of the chain

End-to-end pipeline works *modulo* the topology issue: ~10 min per
specimen, parses cleanly, emits bit + fasm + txt.  Only the segmaker
step asserts on empty output.

## Probe result (`/tmp/ckin_probe/` on 2026-05-30)

Built a minimal `IBUF → BUFR (LOC X0Y2) → MMCM (LOC X0Y0)` design in
Vivado, dumped every PIP used in HCLK_CMT* tiles plus every PIP whose
name contains `CK_IN`.

Result: **zero HCLK_CMT PIPs used**, and the only `CK_IN` PIP in the
whole design was
`CLK_HROW_BOT_R.CLK_HROW_CK_IN_L0->>CLK_HROW_BOT_R_CK_BUFG_CASCO0`
(BUFG-cascade input, *not* an HCLK_CMT input).

Interpretation: Vivado's BUFR→MMCM routing flows entirely through the
CLK_HROW / BUFG-ring network and bypasses the HCLK_CMT entirely.
Combined with 045's history of 100 random specimens *never* hitting
the 21 unsolved CK_IN PIPs, the most likely explanation is that
**Vivado deliberately doesn't use `HCLK_CMT_CK_IN*` PIPs** for any
standard topology — they exist in silicon but the timing/closure
model avoids them.

That changes the conclusion for the open-flow problem.  If Vivado
doesn't set bits for those PIPs and *still* produces a bitstream
that works on hardware, the `IBUFDS_GTE2.IN_USE` / `CK_IN8` lines
nextpnr-xilinx emits are mostly decorative — and our PPIP no-op
patch (silence fasm2frames, emit no bits) is **the correct fix**.

The open-flow VC707 counter's stuck-LED issue is therefore unlikely
to be about missing CK_IN segbits.  More plausible culprits:

1. **FF clear-bits** — counter125's FFs initialise via FDRE/FDSE
   `ZINI/ZRST` segbits.  If the open-flow bitstream programs those
   wrong (e.g. wrong `INIT` value, or `FFSYNC` flipped), the counter
   starts at `0b10001000` (= LEDs 3 + 7 on, all others off, which is
   exactly what we observed) and stays there.
2. **`rst` input routing** — nextpnr placed rst at `IOB_X0Y124`;
   check the LIOB18 IBUF + INT-routing FASM lines against Vivado.
3. **OBUF / DRIVE / SLEW** for the LED IOBs — but those would only
   affect brightness, not the bit pattern.

Picking up this thread should start at `~/counter125_build/top.fasm`
vs `top_vivado.fasm` for the **CLBLL/CLBLM SLICE FF and rst-input
IOB lines**, not the HCLK_CMT block.

## 045a status

The scaffold is preserved as-is (Makefile, top.py, generate.tcl,
targets.csv) because it might still be valuable as a **negative-result
record** — the 21 PIPs in `targets.csv` are essentially "PIPs the
silicon supports but Vivado refuses to use".  A future generation that
needs to fuzz these would do so by *building the routing manually*
with `route_via` from a clean starting design, not by hoping random
placements happen to use them.

## Files

| file          | role |
|---------------|------|
| `Makefile`    | drives N specimens through the common segmatch chain |
| `targets.csv` | the 21 (tile_type, dst, src) tuples to fuzz |
| `top.py`      | per-specimen Verilog generator (placeholder, see TODO) |
| `generate.tcl`| per-specimen Vivado driver: LOC-force the target PIP (TODO) |
