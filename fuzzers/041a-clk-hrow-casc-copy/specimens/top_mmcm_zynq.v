// MMCM in a top-left CMT feeding a bottom-half BUFG: the MMCM output
// enters the clock row of its region (a CLK_HROW_TOP_R tile) on a
// CK_IN_L* wire and cascades down to the BUFGs
module top(input clk, output led);
  wire clk_i, clk_fb, clk_mm, clk_g;
  IBUF ibuf_i (.I(clk), .O(clk_i));
  MMCME2_ADV #(
    .CLKIN1_PERIOD(10.0),
    .CLKFBOUT_MULT_F(10.0),
    .CLKOUT0_DIVIDE_F(10.0)
  ) mmcm_i (
    .CLKIN1(clk_i), .CLKIN2(1'b0), .CLKINSEL(1'b1),
    .CLKFBIN(clk_fb), .CLKFBOUT(clk_fb),
    .CLKOUT0(clk_mm),
    .RST(1'b0), .PWRDWN(1'b0),
    .DADDR(7'b0), .DI(16'b0), .DWE(1'b0), .DEN(1'b0), .DCLK(1'b0),
    .PSCLK(1'b0), .PSEN(1'b0), .PSINCDEC(1'b0)
  );
  BUFG bufg_i (.I(clk_mm), .O(clk_g));
  reg [23:0] c = 24'd0;
  always @(posedge clk_g) c <= c + 1;
  assign led = c[23];
  (* DONT_TOUCH = "true" *) PS7 ps7_i ();
endmodule
