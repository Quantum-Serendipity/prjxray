# the 0048 scenario: SRCC pad AE13 (bank 10, LIOB33_X0Y77, bottom half,
# two regions below the BUFGs) -> the cascade passes CLK_HROW_BOT_R_X137Y130
set_property PACKAGE_PIN AE13 [get_ports clk]
set_property PACKAGE_PIN AA13 [get_ports led]
set_property IOSTANDARD LVCMOS33 [get_ports clk]
set_property IOSTANDARD LVCMOS33 [get_ports led]
set_property LOC BUFGCTRL_X0Y0 [get_cells bufg_i]
create_clock -period 10 [get_ports clk]
