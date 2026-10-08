# Copyright (C) 2017-2022  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
source "$::env(XRAY_DIR)/utils/utils.tcl"

# NOTE: this fuzzer previously defined its own slow Tcl write_pip_txtdata proc
# that iterated foreach net foreach pip and queried the routing graph per-pip
# (set num_pips [llength [get_nodes -uphill ...]] ...). On a large device like
# xc7vx485t this took hours per specimen (1296 nets * O(nodes) Tcl queries).
# Vivado's built-in `write_pip_txtdata $filename` produces the same 6-column
# output (tile pip src_wire dst_wire pnum pdir) that the prjxray segmakers
# parse, but in seconds. Drop the override; the call at the end of this file
# now resolves to the built-in.

proc make_manual_routes {filename} {
    puts "MANROUTE: Loading routes from $filename"

    set fp [open $filename r]
    foreach line [split [read $fp] "\n"] {
        if {$line eq ""} {
            continue
        }

        puts "MANROUTE: Line: $line"

        # Parse the line
        set fields [split $line " "]
        set net_name [lindex $fields 0]
        set wire_name [lindex $fields 1]

        # Check if that net exists
        if {[get_nets $net_name] eq ""} {
            puts "MANROUTE: net $net_name does not exist"
            continue
        }

        set net [get_nets $net_name]

        # Rip it up
        set_property -quiet FIXED_ROUTE "" $net
        set_property IS_ROUTE_FIXED 0 $net
        route_design -unroute -nets $net

        # Make the route
        set nodes [get_nodes -of_objects [get_wires $wire_name]]
        set status [route_via $net_name [list $nodes] 0]

        # Failure, skip manual routing of this net
        if { $status != 1 } {
            puts "MANROUTE: Manual routing failed!"
            set_property -quiet FIXED_ROUTE "" $net
            set_property IS_ROUTE_FIXED 0 $net
            continue
        }

        puts "MANROUTE: Success!"
    }
}

proc run {} {
    create_project -force -part $::env(XRAY_PART) design design
    read_verilog top.v
    synth_design -top top

    set_property CFGBVS GND [current_design]
    set_property CONFIG_VOLTAGE 1.8 [current_design]
    set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]
    set_property IS_ENABLED 0 [get_drc_checks {AVAL-74}]
    set_property IS_ENABLED 0 [get_drc_checks {PDRC-26}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-4}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-5}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-13}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-98}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-99}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-105}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-115}]
    set_property IS_ENABLED 0 [get_drc_checks {REQP-144}]

    set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets]

    place_design -directive Quick
    write_checkpoint -force design_before_route.dcp
    make_manual_routes routes.txt
    route_design -directive Quick -preserve
    write_checkpoint -force design.dcp

    write_bitstream -force design.bit
    write_pip_txtdata design.txt
}

run
