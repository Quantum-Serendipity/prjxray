# Enumerate what this Vivado install will actually target.
# Two nouns kept distinct throughout: PARTS (part+package+speed, what get_parts
# returns) and DIES (the silicon, e.g. xc7a100t), which is what the free-tier
# entitlement question is really about.
set parts [get_parts]
puts "### GET_PARTS_COUNT [llength $parts]"

array set famcount {}
array set famdies {}
foreach p $parts {
    set fam [get_property FAMILY [get_parts $p]]
    set dev [get_property DEVICE [get_parts $p]]
    incr famcount($fam)
    set famdies($fam,$dev) 1
}
puts "### FAMILY_TABLE family parts dies"
foreach fam [lsort [array names famcount]] {
    set dies {}
    foreach k [array names famdies "$fam,*"] {
        lappend dies [string range $k [expr {[string length $fam] + 1}] end]
    }
    puts "### FAMILY $fam parts=$famcount($fam) dies=[llength $dies] : [lsort $dies]"
}

puts "### ARTIX7_DIES"
set a7 {}
foreach p $parts {
    set f [get_property FAMILY [get_parts $p]]
    if {$f eq "artix7"} { lappend a7 [get_property DEVICE [get_parts $p]] }
}
puts "### A7 [lsort -unique $a7]"

puts "### SPARTAN7_DIES"
set s7 {}
foreach p $parts {
    set f [get_property FAMILY [get_parts $p]]
    if {$f eq "spartan7"} { lappend s7 [get_property DEVICE [get_parts $p]] }
}
puts "### S7 [lsort -unique $s7]"

puts "### ALL_PARTS_BEGIN"
foreach p [lsort $parts] { puts $p }
puts "### ALL_PARTS_END"

puts "### LICENSE_ENV XILINXD=[expr {[info exists ::env(XILINXD_LICENSE_FILE)] ? $::env(XILINXD_LICENSE_FILE) : "<unset>"}] LM=[expr {[info exists ::env(LM_LICENSE_FILE)] ? $::env(LM_LICENSE_FILE) : "<unset>"}]"
