# Copyright (C) 2017-2020  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# 045a — CK_IN-targeted segbits for HCLK_CMT[_L].
#
# Picks one (tile_type, dst, src) target from targets.csv per specimen
# (round-robin on SPECIMEN env var), then forces a clock net through the
# CK_IN PIP using FIXED_ROUTE.  Mirrors the proven pattern in 045's
# route_todo (loop through nets, find one in the right tile type, set
# FIXED_ROUTE via find_routing_path).  Standard segmaker chain takes it
# from there.
source "$::env(XRAY_DIR)/utils/utils.tcl"

proc load_targets {} {
    set fp [open "$::env(FUZDIR)/targets.csv" r]
    gets $fp _header
    set rows [list]
    for {gets $fp line} {$line != ""} {gets $fp line} {
        lappend rows [split $line ","]
    }
    close $fp
    return $rows
}

proc pick_target_for_specimen {} {
    # Extract specimen number from cwd (e.g. .../specimen_007 -> 7) since
    # prjxray's top_generate chain doesn't export SPECIMEN as an env var.
    set cwd [pwd]
    set tail [file tail $cwd]
    set n 0
    if {![regexp {specimen_(\d+)} $tail _m num]} {
        puts "045a: WARN — could not parse specimen number from cwd $cwd; defaulting to 1"
        set num 1
    }
    set n [expr {$num - 1}]
    set rows [load_targets]
    set row [lindex $rows [expr {$n % [llength $rows]}]]
    puts "045a: specimen=$tail target_row=$n -> $row"
    return $row
}

# Try to force a net through the target PIP using FIXED_ROUTE.
#
# Approach: gather candidate (net, tile) pairs FIRST without touching
# routing, score them cheaply (does the driver site_pin even share a
# clock backbone with the target tile?), and only unroute + FIXED_ROUTE
# the most promising candidate.  If nothing plausible exists in this
# specimen's random placement, give up immediately — leaving the design
# routed as-is — instead of repeatedly unrouting nets and crashing into
# unreachable wires.
proc force_ck_in_route {target_tile_type target_dst target_src} {
    puts "045a: forcing route through $target_tile_type.$target_dst.$target_src"

    # Collect candidates without mutating the design.
    set candidates [list]
    set max_scan 16   ;# bail-out — never scan more than 16 nets / specimen
    set scanned 0
    foreach net [get_nets] {
        if {$scanned >= $max_scan} { break }
        incr scanned
        set hits [get_wires -of_objects $net -filter "TILE_NAME =~ *HCLK_CMT*"]
        if {[llength $hits] == 0} { continue }
        foreach hit $hits {
            set tile [lindex [split $hit /] 0]
            if {[get_property TILE_TYPE [get_tiles $tile]] != $target_tile_type} { continue }
            set src_wire [get_wires "$tile/$target_src"]
            set dst_wire [get_wires "$tile/$target_dst"]
            if {[llength $src_wire] == 0 || [llength $dst_wire] == 0} { continue }
            lappend candidates [list $net $tile $src_wire $dst_wire]
            break  ;# one candidate tile per net is enough
        }
    }
    puts "045a: scanned $scanned nets, found [llength $candidates] candidate (net,tile) pairs"
    if {[llength $candidates] == 0} {
        puts "045a: no candidate net touches a $target_tile_type tile with $target_src and $target_dst reachable — skipping this specimen"
        return 0
    }

    # Try at most 3 candidates.  For each, do ONE find_routing_path check
    # *before* unrouting; only mutate the net if both legs exist.
    set try_limit 3
    set tried 0
    foreach cand $candidates {
        if {$tried >= $try_limit} { break }
        incr tried
        lassign $cand net tile src_wire dst_wire
        set src_node [get_nodes -of_objects $src_wire]
        set dst_node [get_nodes -of_objects $dst_wire]
        if {[llength $src_node] == 0 || [llength $dst_node] == 0} { continue }
        set origin_pin [get_site_pins -filter {DIRECTION == OUT} -of_objects $net]
        if {[llength $origin_pin] == 0} { continue }
        set origin_node [get_nodes -of_objects $origin_pin]

        # Cheap reachability probe BEFORE we destroy any existing routing.
        set leg1 [find_routing_path -to $src_node -from $origin_node]
        if {[llength $leg1] == 0} {
            puts "045a: candidate $net via $tile — driver can't reach src; skipping"
            continue
        }
        set leg2 [find_routing_path -to $dst_node -from $src_node]
        if {[llength $leg2] == 0} {
            puts "045a: candidate $net via $tile — src can't reach dst; skipping"
            continue
        }

        # Both legs check out — commit.
        route_design -unroute -nets $net
        set_property FIXED_ROUTE [concat $leg1 $leg2] $net
        puts "045a: forced $tile/$target_src -> $tile/$target_dst on net $net (candidate $tried)"
        return 1
    }

    puts "045a: tried $tried candidates, none routable for $target_tile_type.$target_dst.$target_src — leaving design unchanged for this specimen"
    return 0
}

proc run {} {
    create_project -force -part $::env(XRAY_PART) design design
    read_verilog top.v
    synth_design -top top

    set_property CFGBVS VCCO [current_design]
    set_property CONFIG_VOLTAGE 3.3 [current_design]
    set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]
    foreach drc {PDRC-29 PDRC-38 REQP-13 REQP-123 REQP-161 REQP-1575
                 REQP-1684 REQP-1712 AVAL-50 AVAL-78 AVAL-81} {
        set_property IS_ENABLED 0 [get_drc_checks $drc]
    }
    set_property CLOCK_DEDICATED_ROUTE FALSE [get_nets]

    place_design -directive Quick
    route_design -directive Quick

    set target [pick_target_for_specimen]
    lassign $target tile_type dst src
    force_ck_in_route $tile_type $dst $src

    route_design -directive Quick -preserve

    write_checkpoint -force design.dcp
    write_bitstream -force design.bit
    write_pip_txtdata design.txt
}

run
