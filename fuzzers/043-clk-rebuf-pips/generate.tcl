# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
source "$::env(XRAY_DIR)/utils/utils.tcl"

# Local write_pip_txtdata override removed — utils.tcl (sourced above)
# has been patched with a bulk-fetch version that is ~4x faster on
# xc7vx485tffg1761-2. Keeping this shadow would re-introduce hours of
# per-net Tcl overhead.

proc write_route_data {filename} {
    set fp [open $filename w]
    foreach net [get_nets -hierarchical] {
        puts $fp "Net $net route:"
        puts $fp [report_route_status -of_objects $net -return_string]
        puts $fp ""
    }
    close $fp
}

proc run {} {
    create_project -force -part $::env(XRAY_PART) design design
    read_verilog top.v
    synth_design -top top

    set_property CFGBVS VCCO [current_design]
    set_property CONFIG_VOLTAGE 3.3 [current_design]
    set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]

    place_design
    route_design

    write_checkpoint -force design.dcp
    write_bitstream -force design.bit
    write_route_data route.txt
    write_pip_txtdata pips.txt
}

run
