// six simultaneous MMCM outputs to six top-half BUFGs: the six clocks
// need six distinct CK_IN_L* spines on the CLK_HROW_TOP_R tile, pushing
// the allocation into the CK_IN_L4..13 range
module top(input clk, output led);
  wire clk_i, clk_fb;
  wire [5:0] clk_mm, clk_g;
  IBUF ibuf_i (.I(clk), .O(clk_i));
  MMCME2_ADV #(
    .CLKIN1_PERIOD(10.0),
    .CLKFBOUT_MULT_F(12.0),
    .CLKOUT0_DIVIDE_F(12.0),
    .CLKOUT1_DIVIDE(11),
    .CLKOUT2_DIVIDE(10),
    .CLKOUT3_DIVIDE(9),
    .CLKOUT4_DIVIDE(8),
    .CLKOUT5_DIVIDE(7)
  ) mmcm_i (
    .CLKIN1(clk_i), .CLKIN2(1'b0), .CLKINSEL(1'b1),
    .CLKFBIN(clk_fb), .CLKFBOUT(clk_fb),
    .CLKOUT0(clk_mm[0]), .CLKOUT1(clk_mm[1]), .CLKOUT2(clk_mm[2]),
    .CLKOUT3(clk_mm[3]), .CLKOUT4(clk_mm[4]), .CLKOUT5(clk_mm[5]),
    .RST(1'b0), .PWRDWN(1'b0),
    .DADDR(7'b0), .DI(16'b0), .DWE(1'b0), .DEN(1'b0), .DCLK(1'b0),
    .PSCLK(1'b0), .PSEN(1'b0), .PSINCDEC(1'b0)
  );
  wire [5:0] msb;
  genvar i;
  generate for (i = 0; i < 6; i = i + 1) begin : g
    BUFG bufg_i (.I(clk_mm[i]), .O(clk_g[i]));
    reg [23:0] c = 24'd0;
    always @(posedge clk_g[i]) c <= c + 1;
    assign msb[i] = c[23];
  end endgenerate
  assign led = ^msb;
  (* DONT_TOUCH = "true" *) PS7 ps7_i ();
endmodule
