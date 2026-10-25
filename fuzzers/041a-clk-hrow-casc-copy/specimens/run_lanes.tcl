# Lane specimens: one synth + place, then the pad net c is routed by Vivado
# once (on its own lane m) and re-routed with FIXED_ROUTE through each
# HCLK_CMT lane n = 4..13 (the same route with MUX_CLK_m renamed to
# MUX_CLK_n), writing one bitstream and one pip dump per lane.
# Env: SPEC_PART, SPEC_V, SPEC_XDC, SPEC_OUT.
set part $::env(SPEC_PART)
set out $::env(SPEC_OUT)
create_project -in_memory -part $part
read_verilog $::env(SPEC_V)
synth_design -top top -part $part
read_xdc $::env(SPEC_XDC)
place_design
route_design

proc dump {dir} {
  file mkdir $dir
  set fp [open $dir/hrow_pips.txt w]
  foreach net [get_nets -hierarchical] {
    foreach pip [get_pips -quiet -of_objects $net] {
      if {[string match "CLK_HROW*" $pip] || [string match "HCLK_CMT*" $pip]} { puts $fp "$net $pip" }
    }
  }
  close $fp
  set fp [open $dir/routes.txt w]
  puts $fp "c [get_property ROUTE [get_nets c]]"
  close $fp
  report_route_status -file $dir/route_status.txt
  write_bitstream -force $dir/v.bit
}

set r0 [get_property ROUTE [get_nets c]]
if {![regexp {HCLK_CMT_MUX_CLK_(\d+)} $r0 -> m]} { puts "SPEC-NO-LANE $r0"; exit 1 }
puts "SPEC-NATURAL-LANE $m"
dump $out/natural
foreach n {4 5 6 7 8 9 10 11 12 13} {
  set r [regsub -all "HCLK_CMT_MUX_CLK_${m}\\M" $r0 "HCLK_CMT_MUX_CLK_${n}"]
  set r [regsub -all "CLK_HROW_CK_IN_L${m}\\M" $r "CLK_HROW_CK_IN_L${n}"]
  route_design -unroute -nets [get_nets c]
  set_property FIXED_ROUTE $r [get_nets c]
  route_design -nets [get_nets c]
  dump $out/lane$n
  set_property FIXED_ROUTE {} [get_nets c]
}
puts "SPEC-DONE $part"
