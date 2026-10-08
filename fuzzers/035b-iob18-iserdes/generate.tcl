# Copyright (C) 2017-2022  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
create_project -force -part $::env(XRAY_PART) design design
read_verilog top.v
synth_design -top top

set_property CFGBVS GND [current_design]
set_property CONFIG_VOLTAGE 1.8 [current_design]
set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]
set_param tcl.collectionResultDisplayLimit 0

set_property IS_ENABLED 0 [get_drc_checks {NSTD-1}]
set_property IS_ENABLED 0 [get_drc_checks {UCIO-1}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-79}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-81}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-84}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-85}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-87}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-85}]
set_property IS_ENABLED 0 [get_drc_checks {AVAL-28}]
# ISERDES/ILOGIC fuzzer deliberately builds "invalid" clocking and D-inversion
# mux configs to observe their bits; suppress the DRCs that would block bitgen.
set_property IS_ENABLED 0 [get_drc_checks {REQP-105}]
set_property IS_ENABLED 0 [get_drc_checks {PDRC-26}]
set_property IS_ENABLED 0 [get_drc_checks {PDRC-158}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-1580}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-1581}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-1582}]
# ISERDESE2-specific DRCs (CLK/CLKDIV topology) the iserdes fuzzer must bypass.
set_property IS_ENABLED 0 [get_drc_checks {NDRV-1}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-98}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-103}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-109}]
set_property IS_ENABLED 0 [get_drc_checks {REQP-111}]

place_design
route_design

write_checkpoint -force design.dcp
write_bitstream -force design.bit
