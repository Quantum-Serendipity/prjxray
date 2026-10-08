# 038-cfg skipped on virtex7

Marked as 'done' by touching `run.ok` to skip on this family.

**Reason**: the underlying 005-tilegrid/cfg fuzzer fails to derive bits info
for `CFG_CENTER_MID_X157Y84` on `xc7vx485tffg1761-2` (Vivado 2020.1). The
two specimens (JTAG_CHAIN=1 vs JTAG_CHAIN=2) produce **bit-identical** frame
data — only the .bit header differs. Segmatch emits `<0 candidates>` and
add_tdb.py writes `bits: {}`, so 038-cfg's generate.py asserts on zero
segments.

This BSCANE2-based fuzzer needs a richer top module on virtex7 to make the
JTAG_CHAIN parameter encoding land in differential config bits. Tracked as
task #17 in the porting effort.

**Counter smoke-test impact**: none — counter.fasm has zero CFG_CENTER
features.
