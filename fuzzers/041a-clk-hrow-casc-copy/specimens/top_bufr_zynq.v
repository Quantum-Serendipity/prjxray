// BUFR in a top-left HCLK_IOI3 feeding a bottom-half BUFG: the regional
// clock enters the CLK_HROW_TOP_R tile on a CK_BUFRCLK_L* wire and
// cascades down to the BUFGs
module top(input clk, output led);
  wire clk_i, clk_r, clk_g;
  IBUF ibuf_i (.I(clk), .O(clk_i));
  BUFR #(.BUFR_DIVIDE("BYPASS")) bufr_i (
    .I(clk_i), .O(clk_r), .CE(1'b1), .CLR(1'b0));
  BUFG bufg_i (.I(clk_r), .O(clk_g));
  reg [23:0] c = 24'd0;
  always @(posedge clk_g) c <= c + 1;
  assign led = c[23];
  (* DONT_TOUCH = "true" *) PS7 ps7_i ();
endmodule
