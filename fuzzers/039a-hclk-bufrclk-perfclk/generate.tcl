# Copyright (C) 2017-2020  The Project X-Ray Authors.
#
# Use of this source code is governed by a ISC-style
# license that can be found in the LICENSE file or at
# https://opensource.org/licenses/ISC
#
# SPDX-License-Identifier: ISC
#
# 039's bitstream path plus a dump of the used pips of the HCLK and CMT
# tiles the rows live on.  The tags are read from those pips, not from
# the placed primitives, so the dump is part of the measurement.
proc dump_used_clock_pips {filename} {
    set fp [open $filename w]
    set nets [get_nets -quiet -hierarchical]
    foreach net $nets {
        foreach pip [get_pips -quiet -of_objects $net] {
            if {[regexp {HCLK_L_|HCLK_R_|HCLK_CMT|CMT_TOP_.*LOWER_B} $pip]} {
                puts $fp "$pip"
            }
        }
    }
    close $fp
}

proc dump_sites {filename filter} {
    set fp [open $filename w]
    foreach c [get_cells -quiet -hierarchical -filter $filter] {
        puts $fp "[get_property NAME $c] [get_sites -quiet -of_objects $c]"
    }
    close $fp
}

# FUZZ_MODE=forced: top.py lists, per line,
#   <net> <MMCM site> <CLK_PERF index> <BUFR site> <source>
# and the route of that net is fixed through the CLK_PERF<n> wire of the
# CMT tile before route_design, MMCM output -> CLK_PERF<n> -> BUFR/I.
# Only the branch to that BUFR is fixed; the CLKFBOUT net also feeds the
# MMCM's own CLKFBIN and that sink is left to the router.
# FUZZ_MODE=forced4 adds a sixth field, the PHSR_PERFCLK<p> wire of the
# HCLK_CMT tile the route goes through; the route is then fixed through that
# wire as well (the unforced path leaves the choice to find_routing_path).
proc force_perf_routes {filename} {
    if {![file exists $filename]} {
        return
    }
    set fp [open $filename r]
    set log [open forced_routes.txt w]
    while {[gets $fp line] >= 0} {
        if {[string trim $line] eq ""} { continue }
        lassign $line netname mmcm mux bufr src target
        set net [get_nets $netname]
        set tile [get_tiles -of_objects [get_sites $mmcm]]
        set wire [get_wires -of_objects $tile -filter "NAME =~ */*CLK_PERF$mux"]
        if {[llength $wire] != 1} {
            error "force: expected one CLK_PERF$mux wire on $tile, got '$wire'"
        }
        set src_pin [get_site_pins -filter {DIRECTION == OUT} -of_objects $net]
        set n0 [get_nodes -of_objects $src_pin]
        set n1 [get_nodes -of_objects $wire]
        set n2 [get_nodes -of_objects [get_site_pins $bufr/I]]
        set r1 [find_routing_path -quiet -from $n0 -to $n1]
        if {$target eq ""} {
            set r2 [find_routing_path -quiet -from $n1 -to $n2]
            if {$r1 eq "" || $r2 eq ""} {
                error "force: no path $n0 -> $n1 -> $n2 for net $net"
            }
            set route [concat $r1 [lrange $r2 1 end]]
        } else {
            # the HCLK_CMT tile the unforced path crosses
            set rn [find_routing_path -quiet -from $n1 -to $n2]
            if {![regexp {(HCLK_CMT[A-Z_]*_X[0-9]+Y[0-9]+)/HCLK_CMT_MUX_PHSR_PERFCLK} $rn -> hcmt]} {
                error "force: no HCLK_CMT tile on the path $rn"
            }
            set pw [get_wires -quiet "$hcmt/HCLK_CMT_MUX_PHSR_PERFCLK$target"]
            if {[llength $pw] != 1} {
                error "force: expected one PHSR_PERFCLK$target wire on $hcmt, got '$pw'"
            }
            set np [get_nodes -of_objects $pw]
            set r2 [find_routing_path -quiet -from $n1 -to $np]
            set r3 [find_routing_path -quiet -from $np -to $n2]
            if {$r1 eq "" || $r2 eq "" || $r3 eq ""} {
                error "force: no path $n0 -> $n1 -> $np -> $n2 for net $net"
            }
            set route [concat $r1 [lrange $r2 1 end] [lrange $r3 1 end]]
        }
        set_property FIXED_ROUTE $route $net
        puts $log "$netname $mmcm CLK_PERF$mux $bufr $src $target $route"
    }
    close $fp
    close $log
}

# What Vivado routed for the forced nets: the CLK_PERF pip of each.
proc check_forced_routes {filename} {
    if {![file exists forced.txt]} {
        return
    }
    set fp [open forced.txt r]
    set out [open $filename w]
    while {[gets $fp line] >= 0} {
        if {[string trim $line] eq ""} { continue }
        lassign $line netname mmcm mux bufr src target
        set hit {}
        set phsr {}
        foreach pip [get_pips -quiet -of_objects [get_nets $netname]] {
            if {[regexp "CLK_PERF$mux\$" $pip]} { lappend hit $pip }
            if {$target ne "" && [regexp "HCLK_CMT_MUX_PHSR_PERFCLK$target\$" $pip]} { lappend phsr $pip }
        }
        set extra ""
        if {$target ne ""} {
            set extra " phsr=[llength $phsr] phsr_pip=$phsr"
        }
        puts $out "$netname CLK_PERF$mux $src pips=[llength $hit] status=[get_property ROUTE_STATUS [get_nets $netname]]$extra $hit"
    }
    close $fp
    close $out
}

proc run {} {
    create_project -force -part $::env(XRAY_PART) design design
    read_verilog top.v
    synth_design -top top

    set_property CFGBVS VCCO [current_design]
    set_property CONFIG_VOLTAGE 3.3 [current_design]
    set_property BITSTREAM.GENERAL.PERFRAMECRC YES [current_design]

    place_design
    force_perf_routes forced.txt
    route_design
    check_forced_routes forced_check.txt

    dump_sites bufr_sites.txt {REF_NAME == BUFR}
    dump_sites bufh_sites.txt {REF_NAME == BUFHCE}
    dump_sites mmcm_sites.txt {REF_NAME =~ MMCME2*}
    dump_used_clock_pips design_pips.txt

    write_checkpoint -force design.dcp
    write_bitstream -force design.bit
}

run
