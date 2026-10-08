# like run.tcl but the XDC is read after synth so net-level constraints
# (CLOCK_DEDICATED_ROUTE) can resolve their get_nets
set part $::env(SPEC_PART)
set top_v $::env(SPEC_V)
set xdc $::env(SPEC_XDC)
set out $::env(SPEC_OUT)
create_project -in_memory -part $part
read_verilog $top_v
synth_design -top top -part $part
read_xdc $xdc
place_design
route_design
write_bitstream -force $out/specimen.bit
set fp [open $out/hrow_pips.txt w]
foreach net [get_nets -hierarchical] {
  foreach pip [get_pips -quiet -of_objects $net] {
    set p "$pip"
    if {[string match "CLK_HROW*" $p]} { puts $fp "$net $p" }
  }
}
close $fp
report_route_status -file $out/route_status.txt
puts "SPEC-DONE $part"
