# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
create_project -force -part $::env(XRAY_PART) design design
#read_verilog $::env(FUZDIR)/top.v
read_verilog top.v
synth_design -top top

set_property -dict "PACKAGE_PIN $::env(XRAY_PIN_00) IOSTANDARD LVCMOS33" [get_ports clk]
set_property -dict "PACKAGE_PIN $::env(XRAY_PIN_01) IOSTANDARD LVCMOS33" [get_ports stb]
set_property -dict "PACKAGE_PIN $::env(XRAY_PIN_02) IOSTANDARD LVCMOS33" [get_ports di]
set_property -dict "PACKAGE_PIN $::env(XRAY_PIN_03) IOSTANDARD LVCMOS33" [get_ports do]

create_pblock roi

add_cells_to_pblock [get_pblocks roi] [get_cells roi]
resize_pblock [get_pblocks roi] -add "$::env(XRAY_ROI)"

set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]

set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets clk_IBUF]
set_property IS_ENABLED 0 [get_drc_checks {DRC NDRV-1}]

# 2025.2 only. This fuzzer LOCs its one BSCANE2 to whichever BSCAN site sorts
# first -- BSCAN_X0Y0 -- and sweeps JTAG_CHAIN over 1..4, because the bits it is
# solving are CFG_CENTER_MID.BSCAN.JTAG_CHAIN_1..4. Vivado 2025.2 refuses three
# quarters of that sweep:
#
#   ERROR: [DRC PDRC-2] BSCAN_JtagChain: Unsupported programming for BSCAN site
#   BSCAN_X0Y0 and JTAG_CHAIN attribute value 3.
#
# Measured on xc7a50tfgg484-1: JTAG_CHAIN=1 builds, 2/3/4 fail at write_bitstream
# with no bitstream produced, so the specimen dies and `make` stops. 2017.2 has
# no such check. The rule is a placement convention (chain n belongs on
# BSCAN_X0Y(n-1)), not a claim that the configuration is unrepresentable -- the
# bits 2017.2 produced for all four values are in the committed database. Turning
# the check off restores the sweep; the resulting segbits are the comparison that
# says whether the encoding is unchanged.
if {[llength [get_drc_checks -quiet PDRC-2]]} {
    set_property IS_ENABLED 0 [get_drc_checks PDRC-2]
}
# Disable MMCM frequency etc sanity checks
#set_property IS_ENABLED 0 [get_drc_checks {PDRC-29}]
#set_property IS_ENABLED 0 [get_drc_checks {PDRC-30}]
#set_property IS_ENABLED 0 [get_drc_checks {AVAL-50}]
#set_property IS_ENABLED 0 [get_drc_checks {AVAL-53}]
#set_property IS_ENABLED 0 [get_drc_checks {REQP-126}]
# PLL
#set_property IS_ENABLED 0 [get_drc_checks {REQP-161}]

place_design
route_design

write_checkpoint -force design.dcp
write_bitstream -force design.bit
