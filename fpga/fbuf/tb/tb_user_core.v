// tb_user_core.v - fbuf user_core: SPI packets in, 15 kHz RGB333 out, UART status back.
//
// Plays the CM0: sends SETREG / PALETTE / PIXELS / FLIP packets on a real
// SCLK, reads the status byte the FPGA sends each field, and checks every
// pixel of the picture window on the RGB pins against a model of both
// buffers and the palette. A small 16 x 8 picture keeps it quick.
`timescale 1ps/1ps
`include "fw2_profile.vh"
module tb_user_core;

localparam integer CLK_PS = 40000;   // 25 MHz
reg clk = 1'b0;
always #(CLK_PS/2) clk = ~clk;

reg        rst_n = 1'b0;
reg  [6:0] reg_addr = 7'd0;
reg  [7:0] reg_wdata = 8'd0;
reg        reg_wr = 1'b0;
wire [7:0] reg_rdata;
reg  [4:0] cm0_in = 5'b11000;        // CS_n high, TXD idle
reg        rx_ready = 1'b1;          // the shell's stream FIFO has room
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
  .fifo_rx_data(fifo_rx_data), .fifo_rx_valid(fifo_rx_valid), .fifo_rx_ready(rx_ready),
  .cm0_in(cm0_in), .cm0_out(cm0_out)
);

integer errors = 0;
task check(input cond, input [8*64-1:0] what);
  if (!cond) begin errors = errors + 1; $display("FAIL: %0s", what); end
endtask

task wreg(input [6:0] a, input [7:0] d);
  begin @(negedge clk); reg_addr = a; reg_wdata = d; reg_wr = 1'b1; @(negedge clk); reg_wr = 1'b0; end
endtask
task rreg(input [6:0] a, output [7:0] d);
  begin @(negedge clk); reg_addr = a; #1; d = reg_rdata; end
endtask
task r32(input [6:0] a, output [31:0] v);
  reg [7:0] b0, b1, b2, b3;
  begin rreg(a, b0); rreg(a + 7'd1, b1); rreg(a + 7'd2, b2); rreg(a + 7'd3, b3); v = {b3, b2, b1, b0}; end
endtask

// ---- geometry under test ----
localparam integer W = 16, H = 8, CPP = 4, XS = 100, YS = 50;
localparam integer HX0 = 235 + XS, VY0 = 19 + YS;

// ---- model ----
reg [7:0] mem_m [0:1][0:W*H-1];
reg [8:0] pal_m [0:255];
integer i, j;
initial for (i = 0; i < 256; i = i + 1) pal_m[i] = {i[7:5], i[4:2], i[1:0], i[1]};

// ---- SPI: one packet per CS-low burst ----
reg [7:0] pkt [0:4095];
integer   half_ps = 10_000;          // 50 MHz
task spi_send(input integer n);
  integer k, bi;
  begin
    cm0_in[4] = 1'b0; #(200_000);
    for (k = 0; k < n; k = k + 1)
      for (bi = 7; bi >= 0; bi = bi - 1) begin
        cm0_in[0] = pkt[k][bi];
        #(half_ps); cm0_in[1] = 1'b1;
        #(half_ps); cm0_in[1] = 1'b0;
      end
    #(100_000); cm0_in[4] = 1'b1; #(200_000);
  end
endtask
task hdr(input [7:0] cmd, input [7:0] arg, input [15:0] param);
  begin pkt[0] = cmd; pkt[1] = arg; pkt[2] = param[7:0]; pkt[3] = param[15:8]; end
endtask
task setreg(input [7:0] r, input [15:0] v); begin hdr(8'h04, r, v); spi_send(4); end endtask
task flip;                                   begin hdr(8'h03, 8'h00, 16'h0000); spi_send(4); end endtask

// Write the whole back buffer (= model buffer `bb`) with idx(x, y) = seed + x + 16 * y
task send_frame(input integer bb, input [7:0] seed);
  integer k;
  begin
    hdr(8'h01, 8'h00, 16'h0000);
    for (k = 0; k < W * H; k = k + 1) begin
      pkt[4 + k] = seed + k[7:0];
      mem_m[bb][k] = seed + k[7:0];
    end
    spi_send(4 + W * H);
  end
endtask

// ---- UART packets from the FPGA (6.25 Mbaud = 160 ns per bit) ----
// 11 bytes: status (bit 7 set), then 10 x 7 input bits. st_last / st_count
// update when a whole packet has arrived; pk_inputs holds its 64 input bits.
reg [7:0]  st_last = 8'h00, st_cur = 8'h00;
integer    st_count = 0, pk_n = -1, pk_bad = 0;
reg [69:0] pk_bits = 70'd0;
reg [63:0] pk_inputs = 64'd0;
always @(negedge cm0_out[0]) begin : uart_rx
  reg [7:0] b;
  integer k;
  #(160_000 + 80_000);
  for (k = 0; k < 8; k = k + 1) begin b[k] = cm0_out[0]; #(160_000); end
  if (cm0_out[0] !== 1'b1) begin errors = errors + 1; $display("FAIL: packet stop bit"); end
  if (b[7]) begin
    if (pk_n >= 0 && pk_n != 10) pk_bad = pk_bad + 1;     // previous packet cut short
    st_cur = b; pk_n = 0;
  end else if (pk_n >= 0 && pk_n < 10) begin
    pk_bits[pk_n * 7 +: 7] = b[6:0];
    pk_n = pk_n + 1;
    if (pk_n == 10) begin
      pk_inputs = pk_bits[63:0];
      st_last = st_cur;
      st_count = st_count + 1;
    end
  end else begin
    pk_bad = pk_bad + 1;                                   // data byte outside a packet
  end
end

// wait until a packet shows front == want (or time out)
task wait_front(input want, input integer max_packets);
  integer start;
  begin
    start = st_count;
    while (st_last[6] !== want && st_count - start < max_packets) @(st_count);
    check(st_last[6] === want, "status packet shows the flip");
  end
endtask

// ---- pin decode and per-pixel check ----
wire [2:0] pin_r = {hdr_out[`FW2_SIG_SPI_SCLK], hdr_out[`FW2_SIG_SPI_MOSI], hdr_out[`FW2_SIG_GPIO26]};
wire [2:0] pin_g = {hdr_out[`FW2_SIG_UART_RTS], hdr_out[`FW2_SIG_SPI_MISO], hdr_out[`FW2_SIG_SPI_CS]};
wire [2:0] pin_b = {hdr_out[`FW2_SIG_UART_TX], hdr_out[`FW2_SIG_UART_RX], hdr_out[`FW2_SIG_UART_CTS]};
wire       pin_csync = hdr_out[`FW2_SIG_GPIO27];
wire [8:0] pin_rgb = {pin_r, pin_g, pin_b};

reg        checking = 1'b0;
reg        ramp_mode = 1'b0;
integer    px_checked = 0, px_bad = 0, border_bad = 0;
always @(negedge clk) begin : pixel_check
  integer h, v, x, y;
  reg [8:0] want;
  h = dut.hcnt - 3;                  // the pins show the pixel from 3 clocks ago
  v = dut.vcnt;
  if (checking && h >= 235 && h < 1551 && v >= 19 && v <= 259) begin
    if (h >= HX0 && h < HX0 + W * CPP && v >= VY0 && v < VY0 + H) begin
      x = (h - HX0) / CPP;
      y = v - VY0;
      want = ramp_mode ? pal_m[x] : pal_m[mem_m[dut.front][y * W + x]];
      px_checked = px_checked + 1;
      if (pin_rgb !== want) begin
        if (px_bad < 5) $display("  pixel (%0d,%0d) buffer %0d: got %03o want %03o", x, y, dut.front, pin_rgb, want);
        px_bad = px_bad + 1;
      end
    end else if (pin_rgb !== 9'd0) begin
      border_bad = border_bad + 1;
    end
  end
end

task wait_field_start; begin @(posedge clk); while (!(dut.hcnt == 0 && dut.vcnt == 0)) @(posedge clk); end endtask
task check_field(input [8*32-1:0] what);
  begin
    wait_field_start;
    px_checked = 0; px_bad = 0; border_bad = 0;
    checking = 1'b1;
    while (dut.vcnt != 9'd261) @(posedge clk);
    checking = 1'b0;
    check(px_checked == W * H * CPP, "every window pixel checked");
    check(px_bad == 0, what);
    check(border_bad == 0, "border black");
    $display("  %0s: %0d pixel clocks, %0d wrong, buffer %0d", what, px_checked, px_bad, dut.front);
  end
endtask

// bytes the core pushes into the stream FIFO
reg [7:0] au_buf [0:1023];
integer   au_n = 0;
always @(posedge clk) if (fifo_rx_valid && rx_ready) begin au_buf[au_n] = fifo_rx_data; au_n = au_n + 1; end

reg [7:0]  v8;
reg [31:0] v32;
integer t0, hs_len;

initial begin
  repeat (4) @(posedge clk);
  rst_n = 1'b1;
  repeat (4) @(posedge clk);

  rreg(7'h12, v8); check(v8 == 8'd3, "revision");
  rreg(7'h18, v8); check(v8 == 8'd0, "WIDTH reset low byte (256)");
  rreg(7'h19, v8); check(v8 == 8'd1, "WIDTH reset high byte (256)");
  rreg(7'h20, v8); check(v8 == 8'd5, "CPP reset");
  wreg(7'h11, 8'h01);                // clear counters

  // ---- hsync width on a normal line ----
  while (!(dut.vcnt == 9'd30 && dut.hcnt == 11'd10)) @(posedge clk);
  @(negedge pin_csync); t0 = $time;
  @(posedge pin_csync); hs_len = ($time - t0) / CLK_PS;
  check(hs_len == 118, "hsync 118 clocks (4.72 us)");
  $display("  hsync %0d clocks", hs_len);

  // ---- geometry over SPI, a palette, frame A into buffer 1, flip ----
  setreg(8'd0, W);  setreg(8'd1, H);  setreg(8'd4, CPP);
  setreg(8'd2, XS); setreg(8'd3, YS);
  rreg(7'h18, v8); check(v8 == W, "SETREG WIDTH");
  rreg(7'h20, v8); check(v8 == CPP, "SETREG CPP");
  hdr(8'h02, 8'h00, 16'd0);          // palette entries 0..7: R7, G7, B7, white, then 1-step greys
  for (i = 0; i < 8; i = i + 1) begin
    pal_m[i] = (i == 0) ? 9'o700 : (i == 1) ? 9'o070 : (i == 2) ? 9'o007 : (i == 3) ? 9'o777 : {3{i[2:0]}};
    pkt[4 + 2 * i] = pal_m[i][7:0];
    pkt[5 + 2 * i] = {7'd0, pal_m[i][8]};
  end
  spi_send(4 + 16);
  send_frame(1, 8'h00);
  check(dut.front == 1'b0, "front is 0 before the flip");
  flip;
  wait_front(1'b1, 3);
  check_field("frame A after flip");

  // ---- frame B into buffer 0 at 66.7 MHz SCLK, flip during vblank ----
  half_ps = 7_500;
  send_frame(0, 8'h40);
  while (!(dut.vcnt == 9'd2)) @(posedge clk);    // inside vblank
  flip;
  check(dut.front == 1'b0, "flip sent in vblank takes effect at once");
  check_field("frame B, 66.7 MHz SCLK");
  half_ps = 10_000;

  // ---- a flip sent mid-picture waits for vblank ----
  send_frame(1, 8'h80);
  while (!(dut.vcnt == 9'd100)) @(posedge clk);
  flip;
  check(dut.front == 1'b0, "flip waits while the picture is drawn");
  while (!(dut.vcnt == 9'd260 && dut.hcnt == 11'd5)) @(posedge clk);
  check(dut.front == 1'b1, "flip happens at line 260");
  check_field("frame C");

  // ---- partial update: 4 pixels at address 2*W + 6 of the back buffer (0) ----
  hdr(8'h01, 8'h00, 2 * W + 6);
  for (i = 0; i < 4; i = i + 1) begin pkt[4 + i] = 8'hF0 + i; mem_m[0][2 * W + 6 + i] = 8'hF0 + i; end
  spi_send(8);
  flip;
  wait_front(1'b0, 3);
  check_field("partial update");

  // ---- unknown command and odd-length packet are harmless ----
  hdr(8'h7E, 8'h00, 16'd0); spi_send(4);
  hdr(8'h01, 8'h00, 16'd0); pkt[4] = 8'h11; pkt[5] = 8'h22; pkt[6] = 8'h33; spi_send(7);  // last byte dropped
  mem_m[1][0] = 8'h11; mem_m[1][1] = 8'h22;
  for (i = 2; i < W * H; i = i + 1) mem_m[1][i] = mem_m[1][i];   // rest of buffer 1 is frame C
  flip;
  wait_front(1'b1, 3);
  check_field("odd packet");

  // ---- test ramp from MAIN ----
  wreg(7'h10, 8'h01); ramp_mode = 1'b1;
  wait_field_start;                  // takes effect at the next field
  check_field("test ramp");
  wreg(7'h10, 8'h00); ramp_mode = 1'b0;

  // ---- inputs: staged until 0x37 is written, then in every packet ----
  for (i = 0; i < 7; i = i + 1) wreg(7'h30 + i, 8'h11 * (i + 1));
  rreg(7'h30, v8); check(v8 == 8'h00, "inputs not committed before 0x37");
  wreg(7'h37, 8'hA5);
  rreg(7'h30, v8); check(v8 == 8'h11, "input byte 0 committed");
  rreg(7'h38, v8); check(v8 == 8'd1, "input update counter");
  j = st_count;
  while (st_count < j + 2) @(st_count);                    // a whole packet sent after the commit
  check(pk_inputs == 64'hA577665544332211, "inputs arrive in the packet");
  $display("  packet inputs %016h, status %02h", pk_inputs, st_last);
  check(pk_bad == 0, "packets well formed");

  // ---- audio: AUDIO packet words become stream FIFO bytes, low byte first ----
  au_n = 0;
  hdr(8'h05, 8'h00, 16'd0);
  for (i = 0; i < 100; i = i + 1) begin pkt[4 + 2 * i] = i[7:0]; pkt[5 + 2 * i] = 8'h80 + i[7:0]; end
  half_ps = 7_500;                   // 66.7 MHz: a word every 6 clocks
  spi_send(4 + 200);
  half_ps = 10_000;
  repeat (20) @(posedge clk);
  check(au_n == 200, "audio bytes pushed");
  j = 0;
  for (i = 0; i < 100; i = i + 1)
    if (au_buf[2 * i] !== i[7:0] || au_buf[2 * i + 1] !== 8'h80 + i[7:0]) j = j + 1;
  check(j == 0, "audio bytes in order");
  $display("  audio: %0d bytes pushed, %0d wrong", au_n, j);
  // FIFO full: bytes are dropped and counted
  rx_ready = 1'b0;
  hdr(8'h05, 8'h00, 16'd0); pkt[4] = 8'h11; pkt[5] = 8'h22; pkt[6] = 8'h33; pkt[7] = 8'h44;
  spi_send(8);
  rx_ready = 1'b1;
  repeat (10) @(posedge clk);
  wreg(7'h11, 8'h02);
  r32(7'h3A, v32); check(v32 == 200, "audio byte counter");
  rreg(7'h3E, v8); check(v8 == 8'd4, "audio drops counted");

  // ---- counters ----
  wreg(7'h11, 8'h02);
  r32(7'h24, v32); check(v32 == 19, "packets"); $display("  packets %0d", v32);
  rreg(7'h2C, v8); check(v8 == 5, "flips");
  rreg(7'h2E, v8); check(v8 == 1, "ignored packets");
  rreg(7'h2F, v8); check(v8 == 0, "no overruns or underruns");
  $display("  status bytes received: %0d, last %02h", st_count, st_last);
  check(st_count >= 8, "a status byte every field");

  if (errors == 0) $display("tb_user_core (fbuf) PASS");
  else             $display("tb_user_core (fbuf) FAIL: %0d errors", errors);
  $finish;
end

initial begin #(400_000_000_000); $display("FAIL: timeout"); $finish; end

endmodule
