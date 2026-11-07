// tb_user_core.v - fastlink user_core: SCLK-clocked SPI receiver, UART RX, UART TX with RTS.
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
// in0 MOSI, in1 SCLK (idle low), in2 RTS_n (low = ready), in3 UART TXD (idle high), in4 CS_n
reg  [4:0] cm0_in = 5'b11000;
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

task snapshot; wreg(7'h11, 8'h02); endtask
task clear;    wreg(7'h11, 8'h01); endtask

function [7:0] lfsr8;
  input [7:0] s;
  integer k;
  begin
    lfsr8 = s;
    for (k = 0; k < 8; k = k + 1)
      lfsr8 = lfsr8[0] ? ({1'b0, lfsr8[7:1]} ^ 8'hB8) : {1'b0, lfsr8[7:1]};
  end
endfunction

// SPI mode 0, MSB first: data changes half a period before each rising edge.
task spi_byte(input [7:0] b, input integer half_ps);
  integer i;
  for (i = 7; i >= 0; i = i - 1) begin
    cm0_in[0] = b[i];
    #(half_ps); cm0_in[1] = 1'b1;
    #(half_ps); cm0_in[1] = 1'b0;
  end
endtask

task cs(input level); begin cm0_in[4] = level; #(1_000_000); end endtask

// UART 8N1 on cm0_in[3], LSB first.
task uart_byte(input [7:0] b, input integer bit_ps);
  integer i;
  begin
    cm0_in[3] = 1'b0; #(bit_ps);
    for (i = 0; i < 8; i = i + 1) begin cm0_in[3] = b[i]; #(bit_ps); end
    cm0_in[3] = 1'b1; #(bit_ps);
  end
endtask

// Receive one 8N1 byte from cm0_out[0].
task uart_get(input integer bit_ps, output [7:0] b);
  integer i;
  begin
    @(negedge cm0_out[0]);
    #(bit_ps + bit_ps/2);
    for (i = 0; i < 8; i = i + 1) begin b[i] = cm0_out[0]; #(bit_ps); end
    check(cm0_out[0] === 1'b1, "TX stop bit");
  end
endtask

// Send n bytes in one CS burst at half_ps, incrementing (lfsr = 0) or LFSR from b.
task spi_burst(inout [7:0] b, input integer n, input integer half_ps, input lfsr);
  integer i;
  begin
    cs(1'b0);
    for (i = 0; i < n; i = i + 1) begin
      spi_byte(b, half_ps);
      b = lfsr ? lfsr8(b) : b + 8'd1;
    end
    #(half_ps); cs(1'b1);
  end
endtask

// Check SPI counters after a run of `n` bytes with `e` expected errors.
task spi_expect(input integer n, input integer e, input [8*24-1:0] what);
  reg [31:0] v;
  begin
    snapshot;
    r32(7'h20, v); check(v == n, what);
    if (v != n) $display("  %0s: %0d bytes, expected %0d", what, v, n);
    r32(7'h24, v); check(v == e, what);
    if (v != e) $display("  %0s: %0d errors, expected %0d", what, v, e);
  end
endtask

reg  [7:0]  v8, b, prev;
reg  [15:0] v16;
reg  [31:0] v32, span;
integer i, n;
real mhz;

initial begin
  repeat (4) @(posedge clk);
  rst_n = 1'b1;
  repeat (4) @(posedge clk);

  // ---- reset values ----
  rreg(7'h14, v8); check(v8 == 8'd1, "revision");
  rreg(7'h10, v8); check(v8 == 8'h00, "control reset");
  check(cm0_out === 2'b01, "outputs idle: TX high, CTS_n low");
  rreg(7'h13, v8); check(v8[4:0] === cm0_in, "live levels");

  // ---- SPI with CS at several rates, incrementing: 3 bursts of 200 bytes each ----
  // half periods: 7 MHz, 12.5 MHz, 25 MHz, 33.3 MHz, 40 MHz (none related to clk)
  for (n = 0; n < 5; n = n + 1) begin
    clear;
    b = 8'hF0;
    for (i = 0; i < 3; i = i + 1)
      spi_burst(b, 200, n == 0 ? 71_428 : n == 1 ? 40_000 : n == 2 ? 20_000 : n == 3 ? 15_000 : 12_500, 1'b0);
    spi_expect(600, 0, "SPI CS incrementing");
    rreg(7'h32, v8); check(v8 == b - 8'd1, "SPI last byte");
    r16(7'h30, v16); check(v16 == 3, "SPI CS bursts");
    $display("  SPI %0s MHz: 600 bytes in 3 bursts ok",
             n == 0 ? "7" : n == 1 ? "12.5" : n == 2 ? "25" : n == 3 ? "33.3" : "40");
  end

  // ---- span and gap: 1000 back-to-back bytes at 25 MHz = 999 * 320 ns ----
  clear;
  b = 8'h00;
  spi_burst(b, 1000, 20_000, 1'b0);
  snapshot;
  r32(7'h28, span);
  mhz = 999.0 * 8.0 / (span * 32.0e-3);   // bits / us
  $display("  span %0d clocks -> %0.2f MHz SCLK measured", span, mhz);
  check(mhz > 24.5 && mhz < 25.5, "SPI span gives 25 MHz");
  r32(7'h2C, v32); check(v32 >= 9 && v32 <= 12, "SPI max gap ~10 clocks when continuous");

  // a 20 us pause between two bursts shows up as the max gap
  clear;
  b = 8'h00;
  spi_burst(b, 50, 20_000, 1'b0);
  #(18_000_000);
  spi_burst(b, 50, 20_000, 1'b0);
  spi_expect(100, 0, "SPI two bursts");
  r32(7'h2C, v32);
  $display("  max gap %0d clocks (%0.1f us)", v32, v32 * 0.032);
  check(v32 > 600 && v32 < 700, "SPI max gap ~20 us");

  // ---- LFSR pattern at 33.3 MHz ----
  wreg(7'h12, 8'h01);
  clear;
  b = 8'h5A;
  spi_burst(b, 300, 15_000, 1'b1);
  spi_burst(b, 300, 15_000, 1'b1);
  spi_expect(600, 0, "SPI LFSR");
  // one wrong byte in the middle: 2 errors (it, and the byte after it)
  clear;
  cs(1'b0);
  for (i = 0; i < 100; i = i + 1) begin
    spi_byte(i == 50 ? 8'h00 : b, 15_000);
    b = lfsr8(b);
  end
  cs(1'b1);
  spi_expect(100, 2, "SPI LFSR injected error");
  wreg(7'h12, 8'h00);

  // ---- CS realigns: 3 stray bits, CS high, then a clean burst ----
  clear;
  cs(1'b0);
  for (i = 0; i < 3; i = i + 1) begin #(20_000); cm0_in[1] = 1'b1; #(20_000); cm0_in[1] = 1'b0; end
  cs(1'b1);
  b = 8'h10;
  spi_burst(b, 50, 20_000, 1'b0);
  spi_expect(50, 0, "SPI CS realign");

  // ---- no-CS mode: stray bit, idle gap, clean data (CS_n stays high) ----
  wreg(7'h10, 8'h08);
  clear;
  #(20_000); cm0_in[1] = 1'b1; #(20_000); cm0_in[1] = 1'b0;     // 1 stray bit
  #(10_000_000);
  for (i = 0; i < 100; i = i + 1) spi_byte(i[7:0], 20_000);       // 25 MHz
  #(10_000_000);
  for (i = 100; i < 200; i = i + 1) spi_byte(i[7:0], 250_000);    // 2 MHz
  #(10_000_000);
  spi_expect(200, 0, "SPI no-CS realign");
  wreg(7'h10, 8'h00);

  // ---- UART RX at 7.8125 Mbaud, exact and 2% fast/slow, then LFSR ----
  clear;
  for (i = 0; i < 100; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS);
  for (i = 100; i < 200; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS * 98 / 100);
  for (i = 200; i < 300; i = i + 1) uart_byte(i[7:0], 4 * CLK_PS * 102 / 100);
  #(1_000_000);
  snapshot;
  r32(7'h34, v32); check(v32 == 300, "UART bytes");
  r32(7'h38, v32); check(v32 == 0, "UART sequence errors");
  r16(7'h3C, v16); check(v16 == 0, "UART framing errors");
  rreg(7'h3E, v8);  check(v8 == 8'd43, "UART last (299 mod 256)");
  r32(7'h40, span); check(span > 299 * 40 * 95 / 100 && span < 299 * 40 * 105 / 100, "UART span");
  $display("  UART RX: 300 bytes, span %0d clocks", span);

  wreg(7'h12, 8'h02);
  clear;
  b = 8'h01;
  for (i = 0; i < 100; i = i + 1) begin uart_byte(b, 4 * CLK_PS); b = lfsr8(b); end
  #(1_000_000);
  snapshot;
  r32(7'h34, v32); check(v32 == 100, "UART LFSR bytes");
  r32(7'h38, v32); check(v32 == 0, "UART LFSR errors");

  // ---- SPI and UART at the same time (25 MHz SPI, LFSR on both) ----
  wreg(7'h12, 8'h03);
  clear;
  fork
    begin b = 8'h33; spi_burst(b, 400, 20_000, 1'b1); end
    begin : uart_side
      reg [7:0] u;
      integer j;
      u = 8'h77;
      for (j = 0; j < 80; j = j + 1) begin uart_byte(u, 4 * CLK_PS); u = lfsr8(u); end
    end
  join
  #(1_000_000);
  spi_expect(400, 0, "SPI alongside UART");
  r32(7'h34, v32); check(v32 == 80, "UART alongside SPI");
  r32(7'h38, v32); check(v32 == 0, "UART alongside SPI errors");
  wreg(7'h12, 8'h00);

  // ---- UART framing error ----
  clear;
  cm0_in[3] = 1'b0; #(10 * 4 * CLK_PS); cm0_in[3] = 1'b1; #(20 * 4 * CLK_PS);
  snapshot;
  r16(7'h3C, v16); check(v16 == 1, "UART framing error counted");

  // ---- UART TX to the CM0 (LFSR), CTS, and waiting for RTS ----
  wreg(7'h12, 8'h04);
  clear;
  wreg(7'h10, 8'h07);                // TX on, wait for RTS, CTS_n high
  check(cm0_out[1] === 1'b1, "CTS_n follows control bit 2");
  uart_get(4 * CLK_PS, prev);
  for (i = 0; i < 20; i = i + 1) begin
    uart_get(4 * CLK_PS, b);
    check(b == lfsr8(prev), "TX LFSR sequence");
    prev = b;
  end
  cm0_in[2] = 1'b1;                  // CM0 not ready: TX stops after the current byte
  #(12 * 4 * CLK_PS);
  snapshot; r32(7'h44, v32);
  #(40 * 4 * CLK_PS);
  check(cm0_out[0] === 1'b1, "TX idle while RTS_n high");
  snapshot; r32(7'h44, span);
  check(span == v32, "TX paused while RTS_n high");
  cm0_in[2] = 1'b0;
  uart_get(4 * CLK_PS, b);
  // a byte may have started just as RTS_n rose; the testbench missed it then
  check(b == lfsr8(prev) || b == lfsr8(lfsr8(prev)), "TX resumes the sequence");
  $display("  UART TX: %0d bytes, paused on RTS_n and resumed", v32);
  wreg(7'h10, 8'h00);
  #(20 * 4 * CLK_PS);
  check(cm0_out === 2'b01, "TX idle after disable");

  if (errors == 0) $display("tb_user_core (fastlink) PASS");
  else             $display("tb_user_core (fastlink) FAIL: %0d errors", errors);
  $finish;
end

initial begin #(200_000_000_000); $display("FAIL: timeout"); $finish; end

endmodule
