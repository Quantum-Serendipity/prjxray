# SRCC pad AC12 (bank 12, LIOB33_X0Y27, bottom-most region) -> the cascade
# passes CLK_HROW_BOT_R_X102Y78 on its way to the bottom BUFGs
set_property PACKAGE_PIN AC12 [get_ports clk]
set_property PACKAGE_PIN AA12 [get_ports led]
set_property IOSTANDARD LVCMOS33 [get_ports clk]
set_property IOSTANDARD LVCMOS33 [get_ports led]
set_property LOC BUFGCTRL_X0Y0 [get_cells bufg_i]
create_clock -period 10 [get_ports clk]
