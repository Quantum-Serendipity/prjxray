// four pad clocks through the four BUFRs of one left HCLK_IOI3 into four
// BUFGs: each BUFR enters the CLK_HROW tile on its own CK_BUFRCLK_L* wire
module top(input [3:0] clk, output led);
  wire [3:0] c, r, g, msb;
  genvar i;
  generate for (i = 0; i < 4; i = i + 1) begin : b
    IBUF ibuf_i (.I(clk[i]), .O(c[i]));
    BUFR #(.BUFR_DIVIDE("BYPASS")) bufr_i (.I(c[i]), .O(r[i]), .CE(1'b1), .CLR(1'b0));
    BUFG bufg_i (.I(r[i]), .O(g[i]));
    (* DONT_TOUCH = "true" *) reg [3:0] q = 4'd0;
    always @(posedge g[i]) q <= q + 1'b1;
    assign msb[i] = q[3];
  end endgenerate
  assign led = ^msb;
endmodule
