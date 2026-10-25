# xc7z100ffg900-1: pad R27 (bank 13) into a top BUFG, one clock row above the
# BUFG row: CLK_HROW_TOP_R_X176Y234 -> X176Y182 -> CLK_BUFG_TOP_R
set_property -dict {PACKAGE_PIN R27 IOSTANDARD LVCMOS33} [get_ports clk]
set_property -dict {PACKAGE_PIN AA13 IOSTANDARD LVCMOS33} [get_ports led]
set_property LOC BUFGCTRL_X0Y16 [get_cells bg]
