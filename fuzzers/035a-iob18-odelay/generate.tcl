# Copyright (C) 2017-2023  The Project X-Ray Authors
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
# ODELAY VAR_LOAD/VAR_LOAD_PIPE leaves CNTVALUEIN intentionally unconnected for
# fuzzing; Vivado 2020.1 enforces this via REQP-135 (the IDELAY equivalents
# REQP-79/81/.. are already disabled above, which is why the idelay fuzzer passes).
set_property IS_ENABLED 0 [get_drc_checks {REQP-135}]
set_property IS_ENABLED 0 [get_drc_checks {AVAL-28}]

place_design
route_design

write_checkpoint -force design.dcp
write_bitstream -force design.bit
