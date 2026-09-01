# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
source "$::env(XRAY_DIR)/utils/utils.tcl"

# 2025.2. This fuzzer LOCs one BSCANE2 to BSCAN_X0Y0 and toggles JTAG_CHAIN
# between "1" and "2"; that toggle IS the fuzz variable whose solved bit
# position gives CFG_CENTER_MID its tilegrid base address. 2025.2 refuses the
# "2" specimen:
#
#   ERROR: [DRC PDRC-2] BSCAN_JtagChain: Unsupported programming for BSCAN site
#   BSCAN_X0Y0 and JTAG_CHAIN attribute value 2.
#
# so half the two-specimen set never reaches write_bitstream and the tdb is
# never produced -- which is how CFG_CENTER_MID loses its `bits` object. The
# check has to be disabled on an open design, and generate_top runs the whole
# flow in one call, so its body is inlined here rather than editing utils.tcl
# for the ~30 fuzzers that share it. Kept verbatim apart from the guard.
create_project -force -part $::env(XRAY_PART) design design
read_verilog top.v
synth_design -top top

set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]

if {[llength [get_drc_checks -quiet PDRC-2]]} {
    set_property IS_ENABLED 0 [get_drc_checks PDRC-2]
}

place_design
route_design

write_checkpoint -force design.dcp
write_bitstream -force design.bit
