// pad-clocked blinky; the dummy PS7 satisfies the Zynq bitstream DRC
module top(input clk, output led);
  wire clk_i, clk_g;
  IBUF ibuf_i (.I(clk), .O(clk_i));
  BUFG bufg_i (.I(clk_i), .O(clk_g));
  reg [23:0] c = 24'd0;
  always @(posedge clk_g) c <= c + 1;
  assign led = c[23];
  (* DONT_TOUCH = "true" *) PS7 ps7_i ();
endmodule
