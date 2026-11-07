// tb_user_core.v - linktest user_core: edge counters, SPI RX, UART RX, UART TX.
`timescale 1ps/1ps
module tb_user_core;

localparam integer CLK_PS = 32000;   // 31.25 MHz

reg clk = 1'b0;
always #(CLK_PS/2) clk = ~clk;

reg        rst_n = 1'b0;
reg  [6:0] reg_addr = 7'd0;
reg  [7:0] reg_wdata = 8'd0;
reg        reg_wr = 1'b0;
wire [7:0] reg_rdata;
reg  [4:0] cm0_in = 5'b00100;        // UART line idles high
wire [1:0] cm0_out;
wire [9:0] hdr_out, hdr_oe, rp_out;
wire       fifo_tx_ready, fifo_rx_valid;
wire [7:0] fifo_rx_data;

user_core dut (
  .clk(clk), .rst_n(rst_n),
  .hdr_in(10'd0), .hdr_out(hdr_out), .hdr_oe(hdr_oe),
  .rp_in(10'd0), .rp_out(rp_out),
  .reg_addr(reg_addr), .reg_wdata(reg_wdata), .reg_wr(reg_wr), .reg_rd(1'b0), .reg_rdata(reg_rdata),
  .fifo_tx_data(8'd0), .fifo_tx_valid(1'b0), .fifo_tx_ready(fifo_tx_ready),
  .fifo_rx_data(fifo_rx_data), .fifo_rx_valid(fifo_rx_valid), .fifo_rx_ready(1'b0),
  .cm0_in(cm0_in), .cm0_out(cm0_out)
);

integer errors = 0;
task check(input cond, input [8*64-1:0] what);
  if (!cond) begin errors = errors + 1; $display("FAIL: %0s", what); end
endtask

task wreg(input [6:0] a, input [7:0] d);
  begin
    @(negedge clk); reg_addr = a; reg_wdata = d; reg_wr = 1'b1;
    @(negedge clk); reg_wr = 1'b0;
  end
endtask

task rreg(input [6:0] a, output [7:0] d);
  begin @(negedge clk); reg_addr = a; #1; d = reg_rdata; end
endtask

task r16(input [6:0] a, output [15:0] v);
  reg [7:0] lo, hi;
  begin rreg(a, lo); rreg(a + 7'd1, hi); v = {hi, lo}; end
endtask

task r32(input [6:0] a, output [31:0] v);
  reg [15:0] lo, hi;
  begin r16(a, lo); r16(a + 7'd2, hi); v = {hi, lo}; end
endtask

task snapshot; wreg(7'h1C, 8'h02); endtask
task clear;    wreg(7'h1C, 8'h01); endtask

// SPI mode 0 on cm0_in[1] (SCLK) and cm0_in[0] (data), MSB first.
task spi_byte(input [7:0] b, input integer half_ps);
  integer i;
  for (i = 7; i >= 0; i = i - 1) begin
    cm0_in[0] = b[i];
    #(half_ps); cm0_in[1] = 1'b1;
    #(half_ps); cm0_in[1] = 1'b0;
  end
endtask

// UART 8N1 on cm0_in[2], LSB first.
task uart_byte(input [7:0] b, input integer bit_ps);
  integer i;
  begin
    cm0_in[2] = 1'b0; #(bit_ps);
    for (i = 0; i < 8; i = i + 1) begin cm0_in[2] = b[i]; #(bit_ps); end
    cm0_in[2] = 1'b1; #(bit_ps);
  end
endtask

// Receive one 8N1 byte from cm0_out[0] at bit_ps.
task uart_get(input integer bit_ps, output [7:0] b);
  integer i;
  begin
    @(negedge cm0_out[0]);
    #(bit_ps + bit_ps/2);
    for (i = 0; i < 8; i = i + 1) begin b[i] = cm0_out[0]; #(bit_ps); end
    check(cm0_out[0] === 1'b1, "TX stop bit");
  end
endtask

reg  [7:0]  v8;
reg  [15:0] v16;
reg  [31:0] v32;
reg  [7:0]  b, prev;
integer i, n;

initial begin
  repeat (4) @(posedge clk);
  rst_n = 1'b1;
  repeat (4) @(posedge clk);

  // ---- reset values ----
  rreg(7'h1D, v8); check(v8 == 8'd1, "revision");
  rreg(7'h20, v8); check(v8 == 8'h01, "SPI select reset");
  rreg(7'h21, v8); check(v8 == 8'h80, "SPI CS reset");
  rreg(7'h31, v8); check(v8 == 8'd4, "UART div reset");
  check(cm0_out === 2'b01, "outputs idle: TX high, CTS_n low");

  // ---- edge counters: input k toggles 10*(k+1) times ----
  clear;
  for (n = 0; n < 5; n = n + 1)
    if (n != 2)
      for (i = 0; i < 10 * (n + 1); i = i + 1) begin cm0_in[n] = ~cm0_in[n]; #(200_000); end
  for (i = 0; i < 30; i = i + 1) begin cm0_in[2] = ~cm0_in[2]; #(200_000); end
  snapshot;
  for (n = 0; n < 5; n = n + 1) begin
    r16(7'h10 + 2 * n, v16);
    check(v16 == 10 * (n + 1), "edge count");
    $display("  input %0d: %0d edges", n, v16);
  end
  rreg(7'h1A, v8); check(v8[4:0] === cm0_in, "live levels");
  cm0_in = 5'b00100;

  // ---- SPI at 7 MHz (71.4 ns half period, the stock bridge rate): 600 bytes in 3 transfers ----
  clear;
  b = 8'hF0;
  for (n = 0; n < 3; n = n + 1) begin
    for (i = 0; i < 200; i = i + 1) begin spi_byte(b, 71_428); b = b + 8'd1; end
    #(10_000_000);                                   // 10 us gap between transfers
  end
  snapshot;
  r32(7'h24, v32); check(v32 == 600, "SPI bytes"); $display("  SPI 7 MHz: %0d bytes", v32);
  r32(7'h28, v32); check(v32 == 0, "SPI errors");
  rreg(7'h2C, v8);  check(v8 == b - 8'd1, "SPI last");

  // ---- SPI at 7.8 MHz, one wrong byte: expect 2 sequence errors ----
  clear;
  for (i = 0; i < 100; i = i + 1) spi_byte(i == 50 ? 8'hAA : i[7:0], 64_000);
  #(10_000_000);
  snapshot;
  r32(7'h24, v32); check(v32 == 100, "SPI 7.8 MHz bytes");
  r32(7'h28, v32); check(v32 == 2, "SPI injected errors");
  $display("  SPI 7.8 MHz: 100 bytes, %0d errors (2 expected)", v32);

  // ---- SPI realign: a stray SCLK pulse, then a gap, then clean data ----
  clear;
  cm0_in[1] = 1'b1; #(64_000); cm0_in[1] = 1'b0;   // 1 stray bit
  #(10_000_000);
  for (i = 0; i < 50; i = i + 1) spi_byte(i[7:0], 64_000);
  #(10_000_000);
  snapshot;
  r32(7'h24, v32); check(v32 == 50, "SPI realign bytes");
  r32(7'h28, v32); check(v32 == 0, "SPI realign errors");

  // ---- UART RX at 7.8125 Mbaud (4 clocks/bit), exact and 2% fast/slow ----
  clear;
  for (i = 0; i < 100; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS);
  for (i = 100; i < 200; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS * 98 / 100);
  for (i = 200; i < 300; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS * 102 / 100);
  #(1_000_000);
  snapshot;
  r32(7'h34, v32); check(v32 == 300, "UART bytes"); $display("  UART RX: %0d bytes", v32);
  r32(7'h38, v32); check(v32 == 0, "UART sequence errors");
  r16(7'h3C, v16); check(v16 == 0, "UART framing errors");
  rreg(7'h3E, v8);  check(v8 == 8'd43, "UART last (299 mod 256)");

  // ---- UART RX framing error: stop bit low ----
  clear;
  cm0_in[2] = 1'b0; #(10 * 4 * CLK_PS); cm0_in[2] = 1'b1; #(20 * 4 * CLK_PS);
  snapshot;
  r16(7'h3C, v16); check(v16 == 1, "UART framing error counted");
  r32(7'h34, v32); check(v32 == 0, "no byte from a framing error");

  // ---- UART TX to the CM0, and CTS ----
  wreg(7'h1B, 8'h03);
  check(cm0_out[1] === 1'b1, "CTS_n follows control bit 1");
  uart_get(4 * CLK_PS, prev);
  for (i = 0; i < 20; i = i + 1) begin
    uart_get(4 * CLK_PS, b);
    check(b == prev + 8'd1, "TX sequence");
    prev = b;
  end
  $display("  UART TX: 21 bytes, last %02h", prev);
  wreg(7'h1B, 8'h00);
  #(20 * 4 * CLK_PS);
  check(cm0_out === 2'b01, "TX idle after disable");

  if (errors == 0) $display("tb_user_core (linktest) PASS");
  else             $display("tb_user_core (linktest) FAIL: %0d errors", errors);
  $finish;
end

initial begin #(200_000_000_000); $display("FAIL: timeout"); $finish; end

endmodule
