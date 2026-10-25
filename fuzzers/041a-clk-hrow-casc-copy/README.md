# 041a-clk-hrow-casc-copy

Copies the CLK_HROW rows that `041-clk-hrow-pips` resolved on artix7 into
the zynq7 and spartan7 databases, and verifies the copies bit-by-bit against
Vivado specimen bitstreams. It started with the BUFG-cascade rows, hence the
name; it now copies every row the family lacks, and the mask bits of those
rows.

## The hole

`CLK_HROW_BOT_R` / `CLK_HROW_TOP_R` are the same tile type on every
7-series family. The zynq7 and spartan7 databases were each missing a
subset of the rows that the artix7 database has (all of them
`origin:041-clk-hrow-pips`):

| family / tile | missing rows | what they are |
|---|---|---|
| zynq7 `clk_hrow_bot_r` | 32 | `CASCO<i> <- CASCIN<i>`, i = 0..31 (pass-through) |
| zynq7 `clk_hrow_top_r` | 448 | `CASCO<i> <- CK_BUFRCLK_L0..3 / CK_IN_L4..13` (fan-in) |
| zynq7 `clk_hrow_top_r` | 240 | `CK_MUX_OUT_L0..11 / CK_MUX_OUT_R0..11 <- CK_IN_L4..13` (BUFH) |
| zynq7 `clk_hrow_top_r` | 14 | `CLK_HROW_CK_IN_L4..13_ACTIVE`, `CLK_HROW_CK_BUFRCLK_L0..3_ACTIVE` |
| spartan7 `clk_hrow_bot_r` | 32 | `CASCO<i> <- CASCIN<i>`, i = 0..31 (pass-through) |

The first version of this tool copied the cascade rows (the first, second
and last line). The BUFH and `_ACTIVE` rows of zynq7 `clk_hrow_top_r` came
later: they sit on the same left-side wires as the fan-in rows, and the
specimens of the first version already contained their bits (see below).

These wires are how a clock reaches the central BUFGs from a distant
clock region: it enters the h-row of its own region (fan-in row onto
`CASCO<i>`) and hops through every CLK_HROW tile in between
(`CASCO<i> <- CASCIN<i>` pass-through) until it reaches the BUFG row.
Any pad->BUFG route whose pad sits more than one region away from the
BUFGs uses a pass-through row, so on zynq7/spartan7 the resulting
bitstream has a set bit no database row explains, and a FASM that names
the pip does not assemble: `fasm2frames` fails with
`Segment DB CLK_HROW_BOT_R, key
CLK_HROW_BOT_R.CLK_HROW_BOT_R_CK_BUFG_CASCO0.CLK_HROW_BOT_R_CK_BUFG_CASCIN0
not found`. That is exactly what nextpnr-xilinx produces on an
xc7z045ffg900 blinky clocked from pad AE13 (bank 10): the clock crosses
one region boundary on its way to the BUFGs and the design cannot be
assembled with the zynq7 database as it stands.

The `_ACTIVE` rows are the enable of each left-side wire: Vivado sets
`CLK_HROW_CK_IN_L<n>_ACTIVE` (or `..._BUFRCLK_L<n>_ACTIVE`) in every
CLK_HROW tile where that wire carries a clock, and nextpnr-xilinx writes
the `CK_IN` one for every `CK_IN` wire it uses. Without the row the FASM
does not assemble either: a seven-output MMCM clocked from bank 12 of an
xc7z100ffg900, with no constraint at all, is placed in the CMT next to the
BUFG row, its outputs enter `CLK_HROW_TOP_R_X176Y182` on `CK_IN_L0..2` and
`CK_IN_L10..13`, and `fasm2frames` fails with `Segment DB CLK_HROW_TOP_R,
key CLK_HROW_TOP_R.CLK_HROW_CK_IN_L10_ACTIVE not found`.

Without the BUFH rows the `CK_IN_L4..13 -> CK_MUX_OUT_*` pips have no
bits, so a left-side clock cannot reach a BUFH on those wires.

## Why copying is sound (and when it would not be)

The rows are not fuzzed here; they are copied from artix7. The premise is
measured, not assumed: every row key these families share with artix7 in
the two segbits files is bit-identical (zynq7: 2836/2836 in bot_r,
2582/2582 in top_r; spartan7: 2836/2836 in both), the target has no row
and no mask bit artix7 lacks, the pips exist in the zynq7/spartan7
`tile_type_CLK_HROW_*.json` (and so do the wires of the `_ACTIVE` rows),
none of the copied bit patterns collides with an existing row, and every
bit the rows use is in the artix7 mask. `copy_rows.py` recomputes all of
that and refuses to write if any of it stops being true. It also
re-canonicalizes the files with `utils/sort_db.py`, so the diff it
produces is insertions-only. It holds the family directory locked while
it runs, and it works out the segbits rows, the origin lines and the mask
bits each on its own, so a run that stopped halfway is completed by the
next one.

```
python3 copy_rows.py zynq7    --db-root <prjxray-db> --apply
python3 copy_rows.py spartan7 --db-root <prjxray-db> --apply
```

Without `--apply` it prints the rows and mask bits it would add and writes
nothing. After both runs the zynq7 and spartan7 `segbits_clk_hrow_*.db`
and `mask_clk_hrow_*.db` are byte-identical to artix7's.

## Specimen verification

`specimens/` holds the designs used to verify the copies against real
Vivado bitstreams (Vivado 2026.1, batch mode; each XDC names its part in a
comment). The bar for a copied row is: the row must be exercised by the
specimen (a routed pip in Vivado's own pip dump, or the `_ACTIVE` row of a
wire such a pip uses), `bit2fasm --verbose` against the unpatched database
must leave exactly the expected unknown bits, and against the patched
database it must decode the feature and leave none -- with the
neighbouring pre-existing rows decoding identically in both, no unknown
bit left anywhere in a CLK_HROW tile, and no copied row decoding that the
specimen did not exercise. `verify_bits.py` automates the comparison:

```
python3 utils/bit2fasm.py --db-root <db-unpatched>/<family> --part <part> \
    --verbose specimen.bit > before.fasm
python3 utils/bit2fasm.py --db-root <db-patched>/<family> --part <part> \
    --verbose specimen.bit > after.fasm
python3 fuzzers/041a-clk-hrow-casc-copy/verify_bits.py <specimen-dir> \
    <db-patched>/<family> <tilegrid.json> <copied-rows.txt>
```

The "no unknown bit left in a CLK_HROW tile" check is new. The first
version of `verify_bits.py` only followed the bits of the rows it had
copied, and so it passed `z045-top-mmcm6` and `z045-top-bufr2` with the
`CK_IN_L4/L5_ACTIVE` and `CK_BUFRCLK_L2_ACTIVE` bits still unknown. Decoded
again with the rows in place, those two specimens resolve exactly those
bits and nothing else.

The pad specimens (`top_pad_*.v` + `z045-bot.xdc` / `z030-bot*.xdc` /
`s100-bot.xdc`) put the clock pad in the bottom-most region with the BUFG
LOCed at the bottom of the device, which makes the cascade hop through
the second CLK_HROW_BOT_R tile -- the pass-through row. On
xc7z045ffg900-1 with pad AE13 this reproduces, pip for pip, the exact
route nextpnr-xilinx emitted in the failing design; the `z030-bot-c1` /
`z030-bot-c15` variants move the BUFG LOC (a bottom `BUFGCTRL_X0Y<k>`
is fed from `CASCO<2k>`) to sample other indices.

The fan-in specimens keep source and BUFG **in the same half**, which is
what makes the route a dedicated one Vivado takes on its own: an MMCM in
a top-left CMT (`top_mmcm_zynq.v` + `z045-top-mmcm2.xdc`) or a BUFR in a
top-left HCLK_IOI3 (`top_bufr_zynq.v` + `z045-top-bufr2.xdc`) driving a
top-half BUFG enters the h-row of its own region on a `CK_IN_L*` /
`CK_BUFRCLK_L*` wire -- the fan-in row -- and cascades down through the
next top_r tile. A single MMCM output lands on `CK_IN_L0` (whose row the
database already had); `top_mmcm6_zynq.v` + `z045-top-mmcm6.xdc` drive
six MMCM outputs to six BUFGs at once, which pushes the spine allocation
up into `CK_IN_L4` / `CK_IN_L5`. Forcing the pair across halves instead
requires `CLOCK_DEDICATED_ROUTE FALSE`, under which the router legally
leaves the cascade for fabric -- those specimens verify nothing and are
not included.

The lane specimens cover every left-side wire one by one, on
xc7z100ffg900-1 (pad R27, bank 13, in the clock row of
`CLK_HROW_TOP_R_X176Y234`). `run_lanes.tcl` places and routes the design
once, then re-routes the pad net `c` with `FIXED_ROUTE` through each
`HCLK_CMT_MUX_CLK_<n>`, n = 4..13 (Vivado's own route with the lane
renamed), and writes one bitstream per lane:

- `top_bufh24_zynq.v` + `z100-bufh24.xdc`: the pad drives the 24 BUFHCEs
  of that tile, so each bitstream exercises the 24
  `CK_MUX_OUT_* <- CK_IN_L<n>` rows and `CK_IN_L<n>_ACTIVE`: 25 rows and
  49 bits per lane, all 250 rows over the ten lanes;
- `top_pad_lane_zynq.v` + `z100-bufg.xdc`: the pad drives a top BUFG,
  so each bitstream exercises `CK_IN_L<n>_ACTIVE` next to the
  pre-existing `CASCO0 <- CK_IN_L<n>` and `CASCO0 <- CASCIN0` rows.

`top_bufr4_zynq.v` + `z100-bufr4.xdc` (with `run.tcl`) drive four BUFGs
from the four BUFRs of `HCLK_IOI3_X1Y234`; Vivado takes one
`CK_BUFRCLK_L<n>` wire per BUFR, which exercises the four
`CK_BUFRCLK_L0..3_ACTIVE` rows.

## Why the left-side hole is zynq7-only, and why 041 could not see it

The left-side sources of a `CLK_HROW_TOP_R` tile are top-left IO and
top-left CMT clocks. On zynq7 the PS occupies the top-left corner:
the small dies (xc7z010/20/30) have no top-left PL IO banks and no
top-left CMT column at all -- on xc7z030 the `CK_IN_L4..13` /
`CK_BUFRCLK_L*` wires of the top_r tiles have no user-reachable driver
(most have no node; one carries only a PSS test clock), and
the four PS clock spines enter on `CK_IN_L0..3`, whose rows the zynq7
database already has. Only the big dies (xc7z035/45/100) have top-left
IOI/CMT tiles, so only there can the fan-in, BUFH and `_ACTIVE` rows of
those wires be exercised -- and a fuzzing population on a small part can
never resolve them. spartan7 devices have top-left IO, which is why
spartan7's top_r file is already complete.
