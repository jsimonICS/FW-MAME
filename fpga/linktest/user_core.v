// user_core.v - linktest: measures the CM0 -> FPGA wires (FW-MAME milestone 1).
//
// The CM0 reaches the FPGA on 5 input nets and 2 output nets (fw2.pcf names
// below). The stock gateware runs a SPI-in / UART-out mailbox on them, but
// which CM0 GPIO drives which net, and how fast each can go, is not
// documented. This design answers both:
//
//   * an edge counter on every input, so the CM0 can toggle each GPIO a
//     unique number of times (cm0/linktest.py pins) and the counts name it;
//   * a SPI mode-0 receiver on any two (or three, with CS) inputs, checking
//     an incrementing byte sequence (cm0/linktest.py spi);
//   * a UART receiver on any input, same check (cm0/linktest.py uart-tx);
//   * a UART transmitter on CM0_RX0 sending an incrementing sequence back
//     (cm0/linktest.py uart-rx), and CM0_CTS0 held low (clear to send).
//
// The inputs are oversampled at the 31.25 MHz design clock through a
// synchronizer, so SPI should work up to about 7.8 MHz SCLK (the stock
// bridge uses 7 MHz). The UART defaults to 4 clocks per bit = 7.8125 Mbaud,
// the CM0's stock rate. (48 MHz would allow faster SPI, but wilifpga's shell
// itself only reaches ~40 MHz.) Needs the FW-MAME wilifpga patch
// ([host] cm0_link = true in design.toml adds the cm0_in / cm0_out ports).
//
// cm0_in bit:  0 CM0_SPI0_MISO  1 CM0_SPI0_CLK  2 CM0_RX1  3 CM0_SPI1_MISO  4 CM0_SPI1_CLK
// cm0_out bit: 0 CM0_RX0 (UART to the CM0)      1 CM0_CTS0 (active low)
//
// Registers (counters read from a snapshot taken by command bit 1, so a
// multi-byte value is consistent; all little-endian):
//   0x10-0x19 r   edge count, 16 bits per input 0..4
//   0x1A      r   live input levels (bits 0-4), not snapshotted
//   0x1B      rw  control: bit 0 UART TX enable, bit 1 CTS_n level. Reset 0.
//   0x1C      w   command: bit 0 clear all counters, bit 1 snapshot. Reads 0.
//   0x1D      r   design revision (1)
//   0x20      rw  SPI select: bits 2:0 SCLK input, bits 6:4 data input. Reset 0x01.
//   0x21      rw  SPI CS: bits 2:0 input (active low), bit 7 = no CS. Reset 0x80.
//   0x24-0x27 r   SPI bytes received (32 bits)
//   0x28-0x2B r   SPI sequence errors (byte != previous + 1)
//   0x2C      r   SPI last byte
//   0x30      rw  UART RX input (bits 2:0). Reset 2.
//   0x31      rw  UART clocks per bit, RX and TX (min 4). Reset 4 (7.8125 Mbaud).
//   0x34-0x37 r   UART bytes received
//   0x38-0x3B r   UART sequence errors
//   0x3C-0x3D r   UART framing errors (16 bits)
//   0x3E      r   UART last byte
//
// Without CS, the SPI receiver realigns to a byte boundary after SCLK has
// been idle for 256 clocks (8.2 us), i.e. between transfers. That limits
// SCLK to at least ~250 kHz.

module user_core (
  input  wire       clk,
  input  wire       rst_n,
  input  wire [9:0] hdr_in,
  output wire [9:0] hdr_out,
  output wire [9:0] hdr_oe,
  input  wire [9:0] rp_in,
  output wire [9:0] rp_out,
  input  wire [6:0] reg_addr,
  input  wire [7:0] reg_wdata,
  input  wire       reg_wr,
  input  wire       reg_rd,
  output wire [7:0] reg_rdata,
  input  wire [7:0] fifo_tx_data,
  input  wire       fifo_tx_valid,
  output wire       fifo_tx_ready,
  output wire [7:0] fifo_rx_data,
  output wire       fifo_rx_valid,
  input  wire       fifo_rx_ready,
  input  wire [4:0] cm0_in,
  output wire [1:0] cm0_out
);

localparam [7:0] REVISION = 8'd1;

assign hdr_out       = 10'd0;
assign hdr_oe        = 10'd0;
assign rp_out        = 10'd0;
assign fifo_tx_ready = 1'b0;
assign fifo_rx_data  = 8'd0;
assign fifo_rx_valid = 1'b0;

// ---- Input synchronizer and edge detect ----
reg [4:0] m1 = 5'd0, m2 = 5'd0, m3 = 5'd0;
always @(posedge clk) begin
  m1 <= cm0_in;
  m2 <= m1;
  m3 <= m2;
end
wire [4:0] toggled = m2 ^ m3;

function pick;   // input idx of v, 0 for idx 5..7
  input [4:0] v;
  input [2:0] idx;
  pick = (idx < 3'd5) ? v[idx] : 1'b0;
endfunction

// ---- Registers ----
reg [1:0] ctrl;
reg [7:0] spi_sel, spi_cs, uart_sel, uart_div;
// Command pulses are registered (one clock late) to keep the bus decode off
// the counters' enable paths.
reg clr = 1'b0, snap = 1'b0;
always @(posedge clk) begin
  clr  <= reg_wr && reg_addr == 7'h1C && reg_wdata[0];
  snap <= reg_wr && reg_addr == 7'h1C && reg_wdata[1];
end

always @(posedge clk) begin
  if (!rst_n) begin
    ctrl     <= 2'b00;
    spi_sel  <= 8'h01;
    spi_cs   <= 8'h80;
    uart_sel <= 8'h02;
    uart_div <= 8'd4;
  end else if (reg_wr) begin
    case (reg_addr)
      7'h1B: ctrl     <= reg_wdata[1:0];
      7'h20: spi_sel  <= reg_wdata;
      7'h21: spi_cs   <= reg_wdata;
      7'h30: uart_sel <= reg_wdata;
      7'h31: uart_div <= (reg_wdata < 8'd4) ? 8'd4 : reg_wdata;
      default: ;
    endcase
  end
end

// ---- Edge counters ----
reg [15:0] ecnt0, ecnt1, ecnt2, ecnt3, ecnt4;
always @(posedge clk) begin
  if (!rst_n || clr) begin
    ecnt0 <= 16'd0; ecnt1 <= 16'd0; ecnt2 <= 16'd0; ecnt3 <= 16'd0; ecnt4 <= 16'd0;
  end else begin
    if (toggled[0]) ecnt0 <= ecnt0 + 16'd1;
    if (toggled[1]) ecnt1 <= ecnt1 + 16'd1;
    if (toggled[2]) ecnt2 <= ecnt2 + 16'd1;
    if (toggled[3]) ecnt3 <= ecnt3 + 16'd1;
    if (toggled[4]) ecnt4 <= ecnt4 + 16'd1;
  end
end

// ---- SPI mode-0 receiver ----
// Selected inputs are registered once more (sclk_a, sdat_a, cs_a) to keep the
// select muxes off the counter paths; all three get the same delay.
reg sclk_a = 1'b0, sclk_b = 1'b0, sdat_a = 1'b0, cs_a = 1'b1;
always @(posedge clk) begin
  sclk_a <= pick(m2, spi_sel[2:0]);
  sdat_a <= pick(m2, spi_sel[6:4]);
  cs_a   <= spi_cs[7] ? 1'b0 : pick(m2, spi_cs[2:0]);
  sclk_b <= sclk_a;
end
wire sclk_rise = sclk_a & ~sclk_b;

reg  [2:0]  spi_bit;
reg  [6:0]  spi_sh;
reg  [8:0]  spi_idle;            // bit 8 set = idle for 256 clocks
reg         spi_v;               // a byte completed last clock: spi_q
reg  [7:0]  spi_q;
reg  [7:0]  spi_expect;
reg         spi_seeded;
reg  [31:0] spi_bytes, spi_errs;
reg  [7:0]  spi_last;

always @(posedge clk) begin
  spi_v <= 1'b0;
  if (!rst_n || clr || cs_a) begin
    spi_bit <= 3'd0;
    if (!rst_n || clr) spi_idle <= 9'd0;
  end else if (sclk_rise) begin
    spi_idle <= 9'd0;
    spi_sh   <= {spi_sh[5:0], sdat_a};
    spi_bit  <= spi_bit + 3'd1;
    if (spi_bit == 3'd7) begin
      spi_v <= 1'b1;
      spi_q <= {spi_sh, sdat_a};
    end
  end else if (!spi_idle[8]) begin
    spi_idle <= spi_idle + 9'd1;
  end else if (spi_cs[7]) begin
    spi_bit <= 3'd0;              // no CS: idle gap = byte boundary
  end
end

always @(posedge clk) begin
  if (!rst_n || clr) begin
    spi_seeded <= 1'b0; spi_bytes <= 32'd0; spi_errs <= 32'd0;
    spi_last <= 8'd0; spi_expect <= 8'd0;
  end else if (spi_v) begin
    spi_bytes  <= spi_bytes + 32'd1;
    spi_last   <= spi_q;
    spi_expect <= spi_q + 8'd1;
    spi_seeded <= 1'b1;
    if (spi_seeded && spi_q != spi_expect) spi_errs <= spi_errs + 32'd1;
  end
end

// ---- UART receiver (8N1, LSB first) ----
reg        rxd = 1'b1;
always @(posedge clk) rxd <= pick(m2, uart_sel[2:0]);
reg  [1:0] rx_state;             // 0 idle, 1 start, 2 data, 3 stop
reg  [7:0] rx_cnt;
reg  [2:0] rx_bit;
reg  [7:0] rx_sh;
reg  [7:0] rx_expect;
reg        rx_seeded;
reg  [31:0] rx_bytes, rx_errs;
reg  [15:0] rx_ferrs;
reg  [7:0]  rx_last;

reg         rx_v = 1'b0, rx_fe = 1'b0;   // byte done / framing error, one clock
reg         rx_wait_hi = 1'b0;

always @(posedge clk) begin
  rx_v  <= 1'b0;
  rx_fe <= 1'b0;
  if (!rst_n || clr) begin
    rx_state <= 2'd0;
    rx_wait_hi <= 1'b0;
  end else begin
    case (rx_state)
      // The start edge is seen 0-1 clocks late, so sampling div/2 - 1 clocks
      // after it lands 25-50% into the bit at div 4: the most margin when the
      // sender is fast, enough (+-2%) when it is slow.
      2'd0: if (rx_wait_hi) rx_wait_hi <= !rxd;   // after a framing error, wait for idle
            else if (!rxd) begin rx_state <= 2'd1; rx_cnt <= {1'b0, uart_div[7:1]} - 8'd2; end
      2'd1: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;   // to mid start bit
            else if (rxd) rx_state <= 2'd0;                 // glitch
            else begin rx_state <= 2'd2; rx_cnt <= uart_div - 8'd1; rx_bit <= 3'd0; end
      2'd2: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;
            else begin
              rx_sh  <= {rxd, rx_sh[7:1]};
              rx_cnt <= uart_div - 8'd1;
              rx_bit <= rx_bit + 3'd1;
              if (rx_bit == 3'd7) rx_state <= 2'd3;
            end
      2'd3: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;
            else begin
              rx_state <= 2'd0;
              rx_v     <= rxd;
              rx_fe    <= !rxd;
              rx_wait_hi <= !rxd;
            end
    endcase
  end
end

// Counters, one clock after the stop bit is sampled.
always @(posedge clk) begin
  if (!rst_n || clr) begin
    rx_seeded <= 1'b0; rx_expect <= 8'd0;
    rx_bytes <= 32'd0; rx_errs <= 32'd0; rx_ferrs <= 16'd0; rx_last <= 8'd0;
  end else if (rx_fe) begin
    rx_ferrs <= rx_ferrs + 16'd1;
  end else if (rx_v) begin
    rx_bytes  <= rx_bytes + 32'd1;
    rx_last   <= rx_sh;
    rx_expect <= rx_sh + 8'd1;
    rx_seeded <= 1'b1;
    if (rx_seeded && rx_sh != rx_expect) rx_errs <= rx_errs + 32'd1;
  end
end

// ---- UART transmitter to the CM0: incrementing bytes while enabled ----
reg [9:0] tx_sh;                 // {stop, data[7:0], start}, shifted out LSB first
reg [3:0] tx_bit;                // bits left in tx_sh
reg [7:0] tx_cnt;
reg [7:0] tx_val;
reg       txd;

always @(posedge clk) begin
  if (!rst_n) begin
    tx_bit <= 4'd0; tx_cnt <= 8'd0; tx_val <= 8'd0; txd <= 1'b1;
  end else if (tx_cnt != 8'd0) begin
    tx_cnt <= tx_cnt - 8'd1;
  end else if (tx_bit != 4'd0) begin
    txd    <= tx_sh[0];
    tx_sh  <= {1'b1, tx_sh[9:1]};
    tx_bit <= tx_bit - 4'd1;
    tx_cnt <= uart_div - 8'd1;
  end else if (ctrl[0]) begin
    tx_sh  <= {1'b1, tx_val, 1'b0};
    tx_bit <= 4'd10;
    tx_val <= tx_val + 8'd1;
  end else begin
    txd <= 1'b1;
  end
end

assign cm0_out = {ctrl[1], txd};

// ---- Snapshot and register read ----
reg [15:0] s_e0, s_e1, s_e2, s_e3, s_e4;
reg [31:0] s_spi_bytes, s_spi_errs, s_rx_bytes, s_rx_errs;
reg [15:0] s_rx_ferrs;
reg [7:0]  s_spi_last, s_rx_last;

always @(posedge clk) begin
  if (snap) begin
    s_e0 <= ecnt0; s_e1 <= ecnt1; s_e2 <= ecnt2; s_e3 <= ecnt3; s_e4 <= ecnt4;
    s_spi_bytes <= spi_bytes; s_spi_errs <= spi_errs; s_spi_last <= spi_last;
    s_rx_bytes <= rx_bytes; s_rx_errs <= rx_errs; s_rx_ferrs <= rx_ferrs; s_rx_last <= rx_last;
  end
end

reg [7:0] rd;
always @* begin
  case (reg_addr)
    7'h10: rd = s_e0[7:0];   7'h11: rd = s_e0[15:8];
    7'h12: rd = s_e1[7:0];   7'h13: rd = s_e1[15:8];
    7'h14: rd = s_e2[7:0];   7'h15: rd = s_e2[15:8];
    7'h16: rd = s_e3[7:0];   7'h17: rd = s_e3[15:8];
    7'h18: rd = s_e4[7:0];   7'h19: rd = s_e4[15:8];
    7'h1A: rd = {3'd0, m2};
    7'h1B: rd = {6'd0, ctrl};
    7'h1D: rd = REVISION;
    7'h20: rd = spi_sel;
    7'h21: rd = spi_cs;
    7'h24: rd = s_spi_bytes[7:0];   7'h25: rd = s_spi_bytes[15:8];
    7'h26: rd = s_spi_bytes[23:16]; 7'h27: rd = s_spi_bytes[31:24];
    7'h28: rd = s_spi_errs[7:0];    7'h29: rd = s_spi_errs[15:8];
    7'h2A: rd = s_spi_errs[23:16];  7'h2B: rd = s_spi_errs[31:24];
    7'h2C: rd = s_spi_last;
    7'h30: rd = uart_sel;
    7'h31: rd = uart_div;
    7'h34: rd = s_rx_bytes[7:0];    7'h35: rd = s_rx_bytes[15:8];
    7'h36: rd = s_rx_bytes[23:16];  7'h37: rd = s_rx_bytes[31:24];
    7'h38: rd = s_rx_errs[7:0];     7'h39: rd = s_rx_errs[15:8];
    7'h3A: rd = s_rx_errs[23:16];   7'h3B: rd = s_rx_errs[31:24];
    7'h3C: rd = s_rx_ferrs[7:0];    7'h3D: rd = s_rx_ferrs[15:8];
    7'h3E: rd = s_rx_last;
    default: rd = 8'd0;
  endcase
end
assign reg_rdata = rd;

/* verilator lint_off UNUSEDSIGNAL */
wire unused = reg_rd | (|hdr_in) | (|rp_in) | (|fifo_tx_data) | fifo_tx_valid | fifo_rx_ready
            | spi_sel[3] | spi_sel[7] | (|spi_cs[6:3]) | (|uart_sel[7:3]) | reg_wdata[7];
/* verilator lint_on UNUSEDSIGNAL */

endmodule
