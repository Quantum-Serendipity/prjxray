# Reproducing the first working VC707 open-flow bitstream

This document captures the end-to-end recipe that produced the first
confirmed-working open-flow bitstream on a real Virtex-7 VC707 board on
**2026-06-01**.  The design — `IBUFDS + BUFG + FDRE + OBUF` — runs from
SYSCLK_P/N at 200 MHz, toggles a flip-flop, and drives LED **D0** (silk
`GPIO_LED_0`, pin `AM39`).  Observed hardware behaviour: D0 at half
brightness from the FF toggling at 100 MHz, with reset (pin `AV40`)
visibly controlling the LED state.

This is the open-flow analogue of what Vivado would produce from the same
RTL — nextpnr-xilinx places and routes, RapidWright converts to a DCP,
Vivado writes the bitstream.

---

## What you need

| Component | Where | Version / branch |
|---|---|---|
| prjxray (this repo) | https://github.com/openXC7/prjxray | `virtex7-support` branch |
| nextpnr-xilinx | https://github.com/openXC7/nextpnr-xilinx | a branch with the patches from `xilinx/{fasm,pack_clocking_xc7,pack_io_xc7}.cc` listed below; develops out of the `atomic-carry4` tag |
| RapidWright | https://github.com/Xilinx/RapidWright | `2025.2.1` standalone jar (`rapidwright-2025.2.1-standalone-lin64.jar`, ~95 MB) plus `gson-2.10.1.jar` |
| json2dcp + WireOracle + BuildWireOracle | local glue | tracked in `deps/json_drc-portable` (git submodule) |
| Vivado | for `write_bitstream` and as source of device data | **2020.1** (others 2018+ may work) |
| SystemVerilog Suite (SVS) | `~/System-Verilog-suite/_build/default/sv_suite.exe` | Verilog → EDIF → nextpnr JSON |
| openFPGALoader | https://github.com/trabucayre/openFPGALoader | built locally; loads via Digilent JTAG |

> **Vivado version.**  `2020.1` is the validated target; the upstream
> prjxray default of `2017.2` is too old for `xc7vx485tffg1761-2`
> (HP-bank IOB18 behaviour, `HCLK_IOI` / `CMT` properties, several
> Tcl helpers that the patched `utils.tcl` exercises).  Set
> `XRAY_VIVADO_SETTINGS=/opt/Xilinx/Vivado/2020.1/settings64.sh`.

---

## The design (`min_ibufds_ff_led`)

`top.v` — minimal IBUFDS + BUFG + FDRE + OBUF:

```verilog
module top (
    input  wire sysclk_p, sysclk_n, rst,
    output wire led
);
    wire clk_raw, clk;
    IBUFDS #(.DIFF_TERM("TRUE"), .IBUF_LOW_PWR("FALSE"), .IOSTANDARD("LVDS"))
        sysclk_ibufds (.I(sysclk_p), .IB(sysclk_n), .O(clk_raw));
    BUFG sysclk_bufg (.I(clk_raw), .O(clk));
    reg q = 1'b0;
    always @(posedge clk) begin
        if (rst) q <= 1'b0;
        else     q <= ~q;
    end
    OBUF led_obuf (.I(q), .O(led));
endmodule
```

`top.xdc` — VC707 pin map:

```tcl
set_property PACKAGE_PIN E19 [get_ports sysclk_p]
set_property PACKAGE_PIN E18 [get_ports sysclk_n]
set_property IOSTANDARD  LVDS [get_ports sysclk_p]
set_property IOSTANDARD  LVDS [get_ports sysclk_n]
create_clock -period 5.000 -name sysclk [get_ports sysclk_p]
set_property PACKAGE_PIN AV40    [get_ports rst]
set_property IOSTANDARD  LVCMOS18 [get_ports rst]
set_property PACKAGE_PIN AM39    [get_ports led]
set_property IOSTANDARD  LVCMOS18 [get_ports led]
set_property CFGBVS GND          [current_design]
set_property CONFIG_VOLTAGE 1.8  [current_design]
```

---

## Build prerequisites

### 1. nextpnr-xilinx

```bash
cd ~/nextpnr-xilinx
cmake -B build -DARCH=xilinx -DBUILD_GUI=OFF
cmake --build build --target nextpnr-xilinx -j$(nproc)
```

Critical patches (in this checkout, on the `virtex7-support` /
`atomic-carry4` branch):

- `xilinx/fasm.cc` — HP-bank glue (LIOB18 default emission, `IBUF_HP_BANK_GLUE`,
  `OBUF_HP_BANK_GLUE`, `IBUFDS_BANK_GLUE`), and the phantom-BUFGCTRL
  filter that suppresses `CLK_BUFG_*_R` features for unused slots.
- `xilinx/pack_clocking_xc7.cc` — preserves `X_ORIG_TYPE=BUFG` and
  `X_ORIG_PORT_I0=I` etc. when packing BUFG → BUFGCTRL so downstream
  EDIF / DCP emission can re-create the BUFG cell.
- `xilinx/pack_io_xc7.cc` — accept `LIOB18_` tiles in addition to
  `RIOB18_` everywhere IOB18-specific placement was hardcoded;
  expand `IBUFDS_GTE2` user check to allow BUFG / BUFH / BUFHCE / BUFR.

The xc7vx485t chipdb must already be built from the `nextpnr-xilinx-meta`
metadata tree (`xilinx/xc7vx485t.bin`).

### 2. RapidWright + the glue jar

The glue lives in `deps/json_drc-portable` and its Makefile fetches
everything it needs — the RapidWright standalone jar, gson, and the
device data for the one part:

```bash
make -C deps/json_drc-portable        # fetch + build all tool jars
make -C deps/json_drc-portable verify # prove the result actually runs
```

Note it does NOT download `rapidwright_data.zip` (2.0 GB); it asks
RapidWright for the single part instead, which is about 3.6 MB.

### 3. Wire-name oracle (one-shot per part)

The oracle is a static per-tile-type table answering PIP-direction,
routethru, and site→tile questions that json2dcp consults at routing-
import time.  Build it once for `xc7vx485tffg1761-2`:

```bash
make -C deps/json_drc-portable oracle/xc7vx485tffg1761-2.oracle.txt.gz
```

(`make deps` builds it too; the rule runs `BuildWireOracle` against the
device database and is slow, so it is cached in `oracle/`.)

Cost: ~1.5 s wall, ~2.5 MB gzipped output.  json2dcp auto-discovers
the file by walking up from the JSON's directory looking for an
`oracle/<part>.oracle.txt.gz` sibling, or via
`XRAY_WIRE_ORACLE=<path>`.  json2dcp **bombs out** if the oracle
file is missing (no fallback — silent fallback produced bitstreams
that loaded but didn't clock on hardware).

---

## End-to-end flow

```bash
cd ~/min_ibufds_ff_led

# 1) SVS:   top.v          -> top_vivado_synth.edif (via Vivado synthesis)
#          top_vivado_synth.edif -> top.json (via SVS)
# 2) nextpnr-xilinx: top.json -> top_routed.json + top.fasm
~/System-Verilog-suite/_build/default/sv_suite.exe script nextpnr_pass/build.lua

# 3) json2dcp: top_routed.json -> top_rw.dcp
ethsoc/routedjson2dcp.sh nextpnr_pass/top_routed.json nextpnr_pass/top_rw.dcp
# (wraps rapidwright_json2dcp.jar, sets RAPIDWRIGHT_PATH + XRAY_WIRE_ORACLE,
#  and preprocesses the nextpnr JSON for json2dcp's strictness)
```

`build.lua` is the SVS script that drives the Verilog→EDIF→JSON→nextpnr
pipeline.  See `nextpnr_pass/build.lua`.

Expected json2dcp output (good run):

```
[wire-oracle] loaded …/xc7vx485tffg1761-2.oracle.txt.gz types=115 tiles=150380 pips=35126
json2dcp ROUTING net=q:        imported=17 (oracle=10, oracle_rev_bidir=0) … lookup_failed=0
json2dcp ROUTING net=rst_IBUF: imported=12 (oracle=6,  oracle_rev_bidir=0) … lookup_failed=0
json2dcp ROUTING net=clk:      imported=13 (oracle=8,  oracle_rev_bidir=3) … lookup_failed=0
json2dcp ROUTING net=clk_raw:  imported=20 (oracle=16, oracle_rev_bidir=2) … lookup_failed=0
```

`lookup_failed=0` on every user net is the success signal.  The
`oracle_rev_bidir` count records how many PIPs needed
`setIsReversed(true)` — those are exactly the bidir-direction cases
that previously failed silently.

### 4. Vivado: DCP → bitstream

Vivado opens the DCP cleanly and reports `pre-route_design`:

| Net | ROUTE_STATUS |
|---|---|
| `clk`, `clk_raw`, `q`, `rst_IBUF` | ROUTED |
| `sysclk_n` | UNROUTED (Vivado fills in) |
| `led`, `rst`, `sysclk_p`, `q_i_1_n_0` | INTRASITE |
| `<const0>`, `<const1>` | ROUTED |

We lock the four good user nets, route `sysclk_n` only, downgrade a few
non-essential DRC checks, and write the bitstream:

```tcl
# /tmp/write_bit_force.tcl
open_checkpoint /home/jonathan/min_ibufds_ff_led/nextpnr_pass/top_rw.dcp

foreach n [get_nets -hier] {
    if {[get_property ROUTE_STATUS $n] == "ROUTED"} {
        set_property IS_ROUTE_FIXED 1 $n
    }
}
catch {route_design -nets [get_nets sysclk_n]} rerr
puts "route_design (sysclk_n only): $rerr"

foreach chk {UCIO-1 RTSTAT-5 RTSTAT-2 RTSTAT-1 NSTD-1 LUTLP-1 CFGBVS-1} {
    catch { set_property IS_ENABLED 0 [get_drc_checks $chk] }
}

write_bitstream -force /home/jonathan/min_ibufds_ff_led/nextpnr_pass/top_rw.bit
exit
```

```bash
vivado -mode batch -nojournal -nolog -source /tmp/write_bit_force.tcl
```

Why these flags:

- `IS_ROUTE_FIXED 1` on ROUTED nets prevents `route_design` from
  unrouting them while it works on `sysclk_n`.  Without the lock,
  `route_design` regresses `q` and `rst_IBUF` to `ANTENNAS` because
  of a separate SLICE pin-assignment CRITICAL (`[Route 35-557]
  q_i_1/I0 A5LUT/A3 not found in siteAssignment`).  That's an open
  bug, not load-bearing for this design.
- `route_design -nets [get_nets sysclk_n]` is a per-net route so it
  doesn't touch the others.
- `IS_ENABLED 0` disables four DRC checks that fire on cosmetic
  issues we know about (XDC LOC application timing, CFGBVS prop,
  LUT loop default, no-IOSTD).

Result: a ~20 MB bitstream at `nextpnr_pass/top_rw.bit`.

### 5. Load on the board

```bash
~/openFPGALoader/build/openFPGALoader \
    --freq 15000000 --cable digilent \
    ~/min_ibufds_ff_led/nextpnr_pass/top_rw.bit
```

`--freq 15000000` is the validated VC707 JTAG clock; the openFPGALoader
default of 6 MHz works too but is slower.  The board must be powered
and the Digilent JTAG cable enumerated (`lsusb` shows a Digilent device).

Expected progress: `Load SRAM: 100.00%` → `Done` → `done 1`.  No errors.

Expected hardware:

- **LED D0** (`GPIO_LED_0`, AM39) dimly oscillating — the FF toggles at
  `sysclk / 2` ≈ 100 MHz, far too fast to see as discrete blinks but
  at ~50 % duty cycle so the LED is at roughly half brightness.
- **Reset** (`CPU_RESET`, AV40) holds the FF in `0` while pressed;
  release returns to the toggling state.

If LED is stuck on, stuck off, or D1 (the next LED over) lights up
too, see [Known issues](#known-issues) below.

---

## What's actually doing the heavy lifting

Three pieces of glue, layered:

1. **`xilinx/fasm.cc` phantom-BUFGCTRL filter (this is what made the
   FASM path work earlier, separately from the DCP work).**  Without
   it, the FASM emits unused-BUFGCTRL features at
   `CLK_BUFG_BOT_R_X192Y204` that program contending clock paths and
   the FF never clocks.
2. **The wire-name oracle (`build/BuildWireOracle.java`,
   `build/WireOracle.java`).**  Without it, json2dcp can't resolve
   bidir-direction PIPs in the clock spine (`CLK_BUFG_REBUF` BIDIR
   pips at Y221 / Y194 / Y169 / Y142) — silently picks the wrong
   direction, route ends up incomplete.
3. **The SITEWIRE→SITEWIRE SitePIP handler in `json2dcp.java`.**
   Without it, the implicit intra-site selections
   (`AOUTMUX:A5Q -> AMUX`, `CLKINV:CLK -> OUT`, `A5FFMUX:IN_A -> OUT`,
   ...) aren't bound on the SiteInst, so the cell pin and the site
   pin are disconnected and Vivado reports `ANTENNAS` on every
   affected net.

---

## Known issues (do not block this design, will trip other designs)

1. **SLICE pin-assignment CRITICAL** —
   `[Route 35-557] resultsCompare: q_i_1/I0 A5LUT/A3 is not found in
   the siteAssignment results from RuleRoute`.  Vivado's
   `route_design` aborts mid-route on `SLICE_X46Y122` because the
   cell-pin → BEL-pin mapping it computes disagrees with what we
   wrote.  Workaround: lock ROUTED nets before `route_design`.

2. **IOB sitetype-net overwrite CRITICALs** — `[Constraints
   18-4866]` fires on `IOB_X0Y124` and `IOB_X1Y276` for paths like
   `OUTBUF_DCIEN_OUT`, `DIFFI_IN`, `PADOUT`.  Default SitePIPs that
   `createAndPlaceCell` binds (e.g. `OUSED:0 -> OUT` on a pure
   IBUF) overlap with our explicit nets.  Cosmetic for this
   design.

3. **UCIO-1 not auto-applied** — XDC LOCs are present in
   `top_rw.dcp/top.xdc` and on the placed cells (`property LOC` in
   EDIF), but Vivado's port-LOC DRC didn't pick them up.  Worked
   around by disabling the DRC; root-cause likely an XDC
   application-order issue.

---

## Glue tree

The json2dcp / WireOracle / BuildWireOracle sources are tracked in
`deps/json_drc-portable/src/` (submodule, github.com/jrrk2/json_drc-portable),
together with dcp2fasm, the opendcp XML tools, and a Makefile that fetches
the upstream jars and device data:

```
build/
  build.sh                  # one-shot compile-and-jar
  manifest.mf
  WireOracle.java           # in-process reader for the oracle file
  BuildWireOracle.java      # offline generator: Device.getDevice() -> oracle
  json2dcp.java             # nextpnr JSON -> RapidWright Design -> DCP
  oracle_classes/           # compiled classes
  rapidwright_json2dcp.jar  # the deliverable
```

That directory is **not** yet in a git repository.  Plan: move it into
this repo under `tools/rapidwright_glue/` (or its own repo on the
openXC7 organisation) so future bisects of the open flow have all
three layers under version control.

---

## Provenance

- Test design lives at `~/min_ibufds_ff_led/`.
- Bitstream artefact: `~/min_ibufds_ff_led/nextpnr_pass/top_rw.bit` (20 MB).
- Oracle artefact: `~/min_ibufds_ff_led/oracle/xc7vx485tffg1761-2.oracle.txt.gz` (2.5 MB).
- Hardware: VC707 Rev1.1 (`xc7vx485tffg1761-2`).
- Vivado: 2020.1.
- nextpnr-xilinx: `virtex7-support` derivative; key commits include
  `4e6663e` (BUFGCTRL default emission), `10c7372` (atomic CARRY4
  packer, also tagged `atomic-carry4`), and the in-tree changes to
  `fasm.cc` + `pack_clocking_xc7.cc` + `pack_io_xc7.cc` referenced
  above.
- RapidWright: `2025.2.1`.
