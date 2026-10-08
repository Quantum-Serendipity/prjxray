# MRCC pad V12 (bank 13, LIOB33_X0Y23, bottom-most region) -> the cascade
# passes CLK_HROW_BOT_R_X73Y78 on its way to the bottom BUFGs
set_property PACKAGE_PIN V12 [get_ports clk]
set_property PACKAGE_PIN AA11 [get_ports led]
set_property IOSTANDARD LVCMOS33 [get_ports clk]
set_property IOSTANDARD LVCMOS33 [get_ports led]
set_property LOC BUFGCTRL_X0Y0 [get_cells bufg_i]
set_property CFGBVS VCCO [current_design]
set_property CONFIG_VOLTAGE 3.3 [current_design]
create_clock -period 10 [get_ports clk]
