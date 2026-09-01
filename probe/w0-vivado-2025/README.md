# Wave-0 probe: does `038-cfg` still yield `CFG_CENTER_MID` bits on Vivado 2025.2?

Run 2026-09-01 on `xc7a50tfgg484-1`, Vivado v2025.2 (`vivado-version.txt`). Verdict, method and
every deviation are written up in the plan repo at
`implementation-plans/xc7-dpr-toolchain-parity-sidecar/artifacts/docs/w0-vivado-2025-cfg-probe.md`.

Short form: **all 11 rows of `prjxray-db/artix7/segbits_cfg_center_mid.db` reproduce bit-for-bit on
2025.2, and the `CFG_CENTER_MID` tilegrid base address resolves to the committed `0x00400900`** —
but only after two source changes, because 2025.2 refuses 14 of `038-cfg`'s 24 specimens and 1 of
`005-tilegrid/cfg`'s 2, and — when that refusal is silenced — ignores the attribute the fuzzer is
sweeping.

Files here:

| file | what |
|---|---|
| `vivado-version.txt` | `vivado -version`, saved. Retires an unsaved-run claim. |
| `get_parts.txt` | `get_parts` on this install: **429**, with a per-family part/die table. Retires the other. |
| `probe_parts.tcl` | the script that produced it. Re-runnable. |
| `pdrc2-refusals.txt` | the `[DRC PDRC-2]` lines, from the unmodified `038-cfg` and `005-tilegrid/cfg` runs. |
| `005-tilegrid.pin_func.2025_2.txt` | 2025.2's `pin_func.txt`. Six lines are wider than two fields; that is what broke `generate.py`. |
| `038-cfg.runC-sitematched.segbits_cfg.db` | the 10 rows `038-cfg` owns, identical to the committed database. |
| `038-cfg.runC-sitematched.segbits_cfg.rdb` | the same, pre-`dbfixup`, showing which tags have no candidates and which never did. |
| `038-cfg-startup.segbits_cfg_center_mid.db` | the 11th row. Bits match; the tile name is doubled by a pre-existing `sed` in that fuzzer's Makefile, not by 2025.2. |
| `005-tilegrid-cfg.runC-sitematched.segbits_tilegrid.tdb` | the solved CFG base address, `0040091B_067_18` → `0x00400900`. |

The fuzzing workspace itself lived in `build_probe/`, which `.gitignore:3` (`build_*`) excludes.
