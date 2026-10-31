# Copyright (C) 2017-2026  The Project X-Ray Authors
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
set part $::env(XRAY_PART)
set samples [expr {[info exists ::env(XRAY_SPEED_SAMPLES)] ? $::env(XRAY_SPEED_SAMPLES) : 3}]

link_design -part $part
set_param messaging.disableStorage 1

set fp [open "part.txt" w]
set p [get_parts $part]
puts $fp "part\t$part"
puts $fp "vivado\t[version -short]"
foreach k {DEVICE PACKAGE SPEED SPEED_LABEL SPEED_LEVEL_ID SPEED_LEVEL_ID_DATE} {
    puts $fp "[string tolower $k]\t[get_property $k $p]"
}
close $fp

set fp [open "models.tsv" w]
foreach sm [get_speed_models] {
    set line [list]
    foreach k {NAME SPEED_INDEX TYPE FAST_MIN FAST_MAX SLOW_MIN SLOW_MAX DRIVE IN_CAP CAP RES WIRE_RES WIRE_CAP FORWARD REVERSE DRIVER} {
        if {[catch {set v [get_property $k $sm]}] || $v eq ""} { set v "-" }
        lappend line $v
    }
    puts $fp [join $line "\t"]
}
close $fp

proc emit {fp kind ttype tile names idx} {
    foreach n $names i $idx {
        puts $fp "$kind\t$ttype\t$tile\t$n\t$i"
    }
}

set fp [open "indices.tsv" w]
foreach ttype [lsort -unique [get_property TYPE [get_tiles]]] {
    set tiles [get_tiles -quiet -filter "TYPE == $ttype"]
    set n [llength $tiles]
    if {$n == 0} { continue }
    set picks [lsort -unique [list 0 [expr {$n / 2}] [expr {$n - 1}]]]
    set picks [lrange $picks 0 [expr {$samples - 1}]]
    foreach k $picks {
        set tile [lindex $tiles $k]
        set pips [get_pips -quiet -of_objects $tile]
        if {[llength $pips]} {
            emit $fp P $ttype $tile [get_property NAME $pips] [get_property SPEED_INDEX $pips]
        }
        set wires [get_wires -quiet -of_objects $tile]
        if {[llength $wires]} {
            emit $fp W $ttype $tile [get_property NAME $wires] [get_property SPEED_INDEX $wires]
        }
        foreach site [get_sites -quiet -of_objects $tile] {
            set stype [get_property SITE_TYPE $site]
            set pins [get_site_pins -quiet -of_objects $site]
            if {![llength $pins]} { continue }
            foreach pin $pins i [get_property SPEED_INDEX $pins] {
                puts $fp "S\t$ttype\t$tile\t$site\t$stype\t$pin\t$i"
            }
        }
    }
}
close $fp
