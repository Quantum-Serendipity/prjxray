// one pad clock into the 24 BUFHCEs of one clock row (12 left, 12 right):
// the clock enters the CLK_HROW tile on one CK_IN_L* wire and fans out to
// every CK_MUX_OUT_L0..11 / CK_MUX_OUT_R0..11
module top(input clk, output led);
  wire c;
  wire [23:0] h, msb;
  IBUF ibuf_i (.I(clk), .O(c));
  genvar i;
  generate for (i = 0; i < 24; i = i + 1) begin : b
    BUFHCE bufh_i (.I(c), .CE(1'b1), .O(h[i]));
    (* DONT_TOUCH = "true" *) reg [3:0] q = 4'd0;
    always @(posedge h[i]) q <= q + 1'b1;
    assign msb[i] = q[3];
  end endgenerate
  assign led = ^msb;
endmodule
