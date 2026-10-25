// pad -> IBUF -> BUFG -> counter bit: one pad clock reaching a BUFG tile that
// is not in its own clock row.
module top(input clk, output led);
	wire c, g;
	IBUF ib(.I(clk), .O(c));
	BUFG bg(.I(c), .O(g));
	reg [23:0] ctr = 0;
	always @(posedge g) ctr <= ctr + 1'b1;
	assign led = ctr[23];
endmodule
