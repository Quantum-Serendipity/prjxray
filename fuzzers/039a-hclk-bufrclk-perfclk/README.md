# 039a-hclk-bufrclk-perfclk

Fuzzer for the HCLK enable buffers of the four regional-clock spokes,
and for the CMT performance-clock path that can feed them.

039 walks the same HCLK_IOI3 tiles, but its BUFRs drive nothing. A BUFR
whose output goes nowhere leaves the HCLK enable at zero, so four rows
of `segbits_hclk_l.db` were never resolvable from that population:

```
HCLK_L.ENABLE_BUFFER.HCLK_CK_BUFRCLK0
HCLK_L.ENABLE_BUFFER.HCLK_CK_BUFRCLK1
HCLK_L.ENABLE_BUFFER.HCLK_CK_BUFRCLK2
HCLK_L.ENABLE_BUFFER.HCLK_CK_BUFRCLK3
```

Here every BUFR that is IN_USE clocks a counter on two SLICEs of its own
clock region, so the regional clock crosses the spine into the fabric.
Part of the population is fed from the region's MMCM instead of from a
clock-capable pad, which is what exercises `CLK_PERF` in
`CMT_TOP_{R,L}_LOWER_B` and `HCLK_CMT_MUX_PHSR_PERFCLK` in
`HCLK_CMT`/`HCLK_CMT_L`.

Families: `hclk_l`, `hclk_r`, `hclk_cmt`, `hclk_cmt_l`,
`cmt_top_r_lower_b`, `cmt_top_l_lower_b`.

## Phase 0: what the tag is allowed to mean

Two negative specimens are built and read **before** the tagging rule is
written down, because the whole campaign depends on the answer.

`neg_unconsumed` places a BUFR and leaves its `O` open: six
`BUFR_Y0.IN_USE`, zero `HCLK_CK_BUFRCLKn->>HCLK_LEAF_CLK_B_*` pips, zero
bits in the four candidate positions. Vivado does not set the enable for
a BUFR that is merely placed.

`neg_ioi` consumes the same BUFR in an ODDR of the IOI column: six
`IN_USE`, six `HCLK_IOI_RCLK2IO2`, six `HCLK_CMT_CK_BUFRCLK2_USED`, and
still zero leaf pips and zero bits. A clock consumed inside the IOI
column never crosses the spine.

So the predicate is the routed leaf pip in that tile, not the BUFR site
and not `IN_USE` — 039's predicate would have tagged both negatives as
ones. Pad-ODDR specimens stay in the randomised population as honest
zeroes rather than being filtered out.

## Specimens

`make` builds `N` (default 50) randomised specimens plus eight fixed
ones:

| Specimen | Shape |
|---|---|
| `cal0` … `cal3` | one-hot: only that BUFRCLK index, pad-fed, on every HCLK_IOI3 |
| `neg_unconsumed` | BUFR placed, `O` open |
| `neg_ioi` | BUFR consumed by an ODDR in the IOI column |
| `neg_unused` | no BUFR |
| `neg_bufh` | BUFHCE into a CLB, no BUFR |

The one-hot specimens are the calibration: each leaves exactly one
unknown bit local to HCLK_L, which is what assigns a bit to an index
rather than to the family. `make gate` fails the run if `BUFRCLK2` does
not come back as `00_31`; nothing is merged when the gate fails.

Two further modes, `perf0` and `perf1`, drive a single MMCM `CLKOUT`
each. They are not part of `make database`: they exist to answer whether
Vivado can be pushed onto a `CLK_PERF0` position, and on xc7a100t it
cannot — it routes `CLKOUTn -> CLK_PERF3 -> MUXED3 -> PERFCLK3` whatever
output is asked for.

## Both CMT columns

`FUZZ_SIDE=L` or `FUZZ_SIDE=R` keeps the MMCM on that column.
`L` is `CMT_TOP_L_LOWER_B` / `HCLK_CMT_L`, `R` is `CMT_TOP_R_LOWER_B` /
`HCLK_CMT`. Unset, each clock region still uses its own single MMCM.

Kintex-7's second column places its BUFRs in `HCLK_IOI` over an HP
`IOI` / `IOB18` bank, not in `HCLK_IOI3`. Those tiles are walked with
the same four neighbour offsets, and the IBUF/OBUF on an `IOB18` uses
`LVCMOS18`. A specimen forced onto one column does not substitute a
pad-fed BUFR on the other column: that BUFR stays unused, so the other
column's `PERFCLK` tags stay zero.

`make database-sides` (default `SIDE_N=32`) builds, per column, 32
randomised specimens plus `specimen_calL0`..`calL3` and
`specimen_calR0`..`calR3`. Each `cal` specimen is `mmcmN`: only
BUFRCLK index N, fed by `CLKOUTN`. With `FUZZ_SIDE` set, a randomised
specimen picks `mmcm_clb` three times out of four (the other draw is
`unused`) so the column under test is actually driven; `CLKOUT` is
still `rel_y % 4`. The default `make database` specimen list, and the
mix used when `FUZZ_SIDE` is unset, are unchanged.

## Directed CLK_PERF2 / CLK_PERF3 population

`make database-forced` (`FORCED_N=64` forced specimens per column, plus
`CTRL_N=8` unforced ones and the four `mmcm` one-hot specimens per
column) exists because the random populations above leave positions of
those two muxes always hot or always cold together, so segmatch cannot
separate their bits.

A `specimen_FL_*` / `specimen_FR_*` specimen (`FUZZ_MODE=forced`, the
column from the name) instantiates, per clock region of the column, the
MMCM on its CMT site, and up to two BUFRs of the region, one per mux:
the source for CLK_PERF2 and for CLK_PERF3 comes from a per-tile shuffled
deck of the 30 usable (source, source) pairs, indexed by the specimen
number. The sources are the five the tile has pips for (`CLKFBOUT`,
`CLKOUT0`..`CLKOUT3`, censused with `get_pips` on the tile); a source is
never used on both muxes of a tile (one net would need two forced
branches) and the all-cold pair is left out. `top.py` writes
`forced.txt`; `generate.tcl` fixes each net's route through the
`CLK_PERF<n>` wire of the CMT tile (`find_routing_path` from the MMCM
output, then to the BUFR input) before `route_design`, and
`forced_check.txt` records the CLK_PERF pip Vivado ended up with for each
forced net. For the `CLKFBOUT` source only the branch to the BUFR is
fixed; the net's other sink, the MMCM's own `CLKFBIN`, is left to the
router.

## Four-mux population: CLK_PERF0..3 and the PHSR_PERFCLK pip behind each

`make database-forced4` (`G_N=80` specimens per column, `CTRL_N=8` unforced
ones numbered from `CTRL_FIRST=101`, and the four `mmcm` one-hot ones) and
`make H_NL=.. H_NR=.. database-forced4-onehot` (the same plus `H_NL` /
`H_NR` one-hot-mux specimens per column) extend the population above to all
four CLK_PERF muxes and fix the HCLK_CMT pip as well.

A `specimen_GL_*` / `specimen_GR_*` specimen (`FUZZ_MODE=forced4`) drives,
per clock region, up to four BUFRs, one per CLK_PERF mux that its
arrangement makes hot. A mux takes one of the five sources and goes through
one of the two PERFCLK of its pair (`CLK_PERF0/1` through `PERFCLK0/1`,
`CLK_PERF2/3` through `PERFCLK2/3`, each PERFCLK at most once); no source
drives two muxes. `forced4_deck()` picks the arrangement of each
specimen by a greedy cover of the cells (source per mux, PERFCLK per mux,
PERFCLK used while the other of its pair is not), shuffled per tile with
a fixed seed, so a run is reproducible and `G_N` can be raised without
changing the earlier specimens. A line of `forced.txt` has a sixth field,
the PERFCLK; `generate.tcl` then fixes the route through that
`HCLK_CMT_MUX_PHSR_PERFCLK<p>` wire as well (the HCLK_CMT tile is the one
the unforced path crosses), and `forced_check.txt` adds `phsr=<n>` and the
PHSR pip.

The greedy deck drives as many muxes as it can, so a position is almost
never hot while the rest of the tile is cold and its own bits cannot be
told from those of the other muxes' states. `specimen_HL_*` /
`specimen_HR_*` (`FUZZ_MODE=forced4h`, `onehot_state()`) drive exactly one
mux of each tile, walking the 20 (mux, source) cells, each through both
PERFCLKs of its group on alternate passes; the tiles of a column are
offset from each other, so one specimen covers several cells.

## Thresholds

`-c 5` for the one-bit enables, as 039 and 058 use for the rest of the
HCLK enables; `-c 2` for the CMT mux positions, as 045 uses. Neither is
lowered to make a family resolve.

## What the fuzzer refuses to publish

`filter_rdb.py` splits the raw segmatch output three ways and writes a
`.notes.txt` for everything it drops, so a refusal is visible rather
than silent:

- `<const0>` tags, never routed in this population, are reported and
  dropped. On xc7a100t all four HCLK_R enables are const0.
- A candidate bit that already belongs to a row of the database is
  stripped by name: `05_21` on `BUFRCLK3` is
  `HCLK_L.HCLK_LEAF_CLK_B_BOTL5.HCLK_CK_BUFRCLK3`, and `29_937` on
  `CLK_PERF3.CLKOUT1` is
  `MMCME2_ADV.CLKOUT1_CLKOUT1_OUTPUT_ENABLE[0]`. A correlation with an
  existing row is not a new row. `CLK_PERF3.CLKOUT1` has nothing left
  after the strip and is dropped entirely.
- A pip the specimens do route for which segmatch finds zero candidate
  bits goes to the notes, not to `build/ppips_*.db` (the file is still
  written, empty). Zero candidates does not mean "default position": see
  the directed population below.

`make pushdb` merges only the rows that carry bits. The fuzzer runs no
`maskmerge`: its population is far too narrow to rewrite masks that 045
and 058 built from theirs.

## Context

Rows: the companion `prjxray-db` PR. Threads:
`openXC7/nextpnr-xilinx#149` (the enables, and the review record for the
phase-0 rule and the calibration gate) and `openXC7/nextpnr-xilinx#172`
(the performance-clock path).
