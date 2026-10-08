# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
proc run {} {
    create_project -force -part $::env(XRAY_PART) design design
    read_verilog top.v
    synth_design -top top

    set_property CFGBVS VCCO [current_design]
    set_property CONFIG_VOLTAGE 3.3 [current_design]
    set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]

    place_design
    route_design

    set fp [open bufio_sites.txt w]
    foreach c [get_cells -quiet -hierarchical -filter {REF_NAME == BUFIO}] {
        puts $fp "[get_property NAME $c] [get_sites -quiet -of_objects $c]"
    }
    close $fp
    write_checkpoint -force design.dcp
    write_bitstream -force design.bit
}

run
