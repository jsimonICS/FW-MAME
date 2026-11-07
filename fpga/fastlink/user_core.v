// user_core.v - fastlink: CM0 -> FPGA SPI receiver clocked by SCLK (FW-MAME milestone 2).
//
// linktest oversampled SCLK at the 31.25 MHz design clock, which tops out at
// 12.5 MHz. Here the shift register is clocked by the CM0's SCLK itself, so
// SPI speed is limited by the wires and the CM0, not by this design. Every
// completed byte crosses into the design clock through a toggle handshake
// (the byte is held for 8 SCLK periods, the handshake needs about 3 clocks,
// so this works up to ~80 MHz SCLK).
//
// Pins are fixed (measured with linktest, see docs/linktest-results.md):
//   cm0_in[0] = GPIO10 SPI0 MOSI      cm0_in[1] = GPIO11 SPI0 SCLK (mode 0)
//   cm0_in[2] = GPIO17 UART RTS_n     cm0_in[3] = GPIO14 UART TXD
//   cm0_in[4] = GPIO21 software CS_n
//   cm0_out[0] = GPIO15 UART RXD      cm0_out[1] = CTS_n (likely GPIO16)
//
// Byte framing: by default CS_n high asynchronously resets the bit counter,
// so every CS-low burst starts on a byte boundary. With control bit 3 set (no
// CS) the bit counter restarts instead after SCLK has been idle for 128
// clocks (4.1 us). That mode has a small race if SCLK resumes exactly as the
// idle timeout fires; the sequence check would show it.
//
// Also: a UART receiver at 7.8125 Mbaud (clk/4) on GPIO14, so SPI and UART can
// be measured together, and a UART transmitter back to the CM0 that can wait
// for the CM0's RTS_n (hardware flow control).
//
// Every receiver checks a byte sequence: next = prev + 1, or next = L(prev)
// with L = 8 steps of the Galois LFSR x^8+x^6+x^5+x^4+1 (period 255 over the
// nonzero bytes), which toggles MOSI far more often than a counter does.
//
// Registers (counters read from a snapshot taken by command bit 1, so a
// multi-byte value is consistent; all little-endian; times in 32 ns clocks):
//   0x10      rw  control: bit 0 UART TX on, bit 1 TX waits for RTS_n low,
//                 bit 2 CTS_n level, bit 3 SPI without CS. Reset 0.
//   0x11      w   command: bit 0 clear counters, bit 1 snapshot. Reads 0.
//   0x12      rw  pattern: bit 0 SPI LFSR, bit 1 UART RX LFSR, bit 2 UART TX LFSR. Reset 0.
//   0x13      r   live input levels (bits 0-4)
//   0x14      r   design revision (1)
//   0x20-0x23 r   SPI bytes received
//   0x24-0x27 r   SPI sequence errors
//   0x28-0x2B r   SPI clocks from the first byte to the last byte
//   0x2C-0x2F r   SPI longest gap between two bytes (clocks)
//   0x30-0x31 r   SPI CS bursts (CS_n rising edges, 16 bits)
//   0x32      r   SPI last byte
//   0x34-0x37 r   UART RX bytes
//   0x38-0x3B r   UART RX sequence errors
//   0x3C-0x3D r   UART RX framing errors (16 bits)
//   0x3E      r   UART RX last byte
//   0x40-0x43 r   UART RX clocks from the first byte to the last byte
//   0x44-0x47 r   UART TX bytes sent

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

// Next byte of the LFSR sequence: 8 steps of a right-shifting Galois LFSR.
function [7:0] lfsr8;
  input [7:0] s;
  integer k;
  begin
    lfsr8 = s;
    for (k = 0; k < 8; k = k + 1)
      lfsr8 = lfsr8[0] ? ({1'b0, lfsr8[7:1]} ^ 8'hB8) : {1'b0, lfsr8[7:1]};
  end
endfunction

// ---- Registers ----
reg [3:0] ctrl;
reg [2:0] pat;
reg clr = 1'b0, snap = 1'b0;      // registered command pulses
always @(posedge clk) begin
  clr  <= reg_wr && reg_addr == 7'h11 && reg_wdata[0];
  snap <= reg_wr && reg_addr == 7'h11 && reg_wdata[1];
end
always @(posedge clk) begin
  if (!rst_n) begin
    ctrl <= 4'd0;
    pat  <= 3'd0;
  end else if (reg_wr) begin
    case (reg_addr)
      7'h10: ctrl <= reg_wdata[3:0];
      7'h12: pat  <= reg_wdata[2:0];
      default: ;
    endcase
  end
end

reg [31:0] now = 32'd0;           // free-running timestamp
always @(posedge clk) now <= now + 32'd1;

// ---- Input synchronizer (design-clock copies of the CM0 nets) ----
reg [4:0] m1 = 5'd0, m2 = 5'd0, m3 = 5'd0;
always @(posedge clk) begin
  m1 <= cm0_in;
  m2 <= m1;
  m3 <= m2;
end

// ======================= SCLK domain =======================
wire sclk   = cm0_in[1];
wire smosi  = cm0_in[0];
wire s_arst = ~ctrl[3] & cm0_in[4];   // CS_n high: hold the bit counter at 0

reg       rq = 1'b0;                  // design clock: toggles to request a realign
reg [2:0] sbit = 3'd0;
reg [6:0] ssh = 7'd0;
reg [7:0] shold = 8'd0;
reg       stog = 1'b0, sack = 1'b0;
wire      realign = rq != sack;       // this SCLK edge is bit 7 (MSB) of a new byte

always @(posedge sclk or posedge s_arst) begin
  if (s_arst) sbit <= 3'd0;
  else        sbit <= realign ? 3'd1 : sbit + 3'd1;
end
always @(posedge sclk) begin
  sack <= rq;
  ssh  <= {ssh[5:0], smosi};
  if (sbit == 3'd7 && !realign) begin
    shold <= {ssh, smosi};
    stog  <= ~stog;
  end
end

// ======================= design clock =======================
// Byte handshake: stog through two flops; shold has been stable for at least
// two clocks when it is captured, and stays so for 8 SCLK periods.
reg t1 = 1'b0, t2 = 1'b0, t3 = 1'b0;
always @(posedge clk) begin
  t1 <= stog;
  t2 <= t1;
  t3 <= t2;
end
wire byte_new = t2 ^ t3;

reg       spi_v = 1'b0;
reg [7:0] spi_q = 8'd0;
always @(posedge clk) begin
  spi_v <= byte_new;
  if (byte_new) spi_q <= shold;
end

// Idle detection for the no-CS mode. Activity = an SCLK edge seen by the
// synchronizer (reliable below ~15 MHz) or a completed byte (frequent above
// ~2 MHz), so no SCLK rate looks idle by aliasing.
reg [7:0] idle = 8'd0;                // bit 7 set = idle for 128 clocks
always @(posedge clk) begin
  if ((m2[1] ^ m3[1]) || byte_new) begin
    idle <= 8'd0;
  end else if (!idle[7]) begin
    idle <= idle + 8'd1;
    if (idle == 8'd126 && ctrl[3]) rq <= ~rq;
  end
end

// CS bursts
reg cs_d = 1'b1;
always @(posedge clk) cs_d <= m2[4];
wire cs_rise = m2[4] & ~cs_d;

// SPI counters
reg  [31:0] spi_bytes, spi_errs, spi_first, spi_last_t, spi_gap, spi_maxgap;
reg  [15:0] spi_bursts;
reg  [7:0]  spi_last, spi_expect;
reg         spi_seeded, spi_gap_v;
always @(posedge clk) begin
  spi_gap_v <= 1'b0;
  if (!rst_n || clr) begin
    spi_bytes <= 32'd0; spi_errs <= 32'd0; spi_first <= 32'd0; spi_last_t <= 32'd0;
    spi_maxgap <= 32'd0; spi_bursts <= 16'd0; spi_last <= 8'd0; spi_expect <= 8'd0;
    spi_seeded <= 1'b0;
  end else begin
    if (cs_rise) spi_bursts <= spi_bursts + 16'd1;
    if (spi_v) begin
      spi_bytes  <= spi_bytes + 32'd1;
      spi_last   <= spi_q;
      spi_expect <= pat[0] ? lfsr8(spi_q) : spi_q + 8'd1;
      spi_seeded <= 1'b1;
      spi_last_t <= now;
      spi_gap    <= now - spi_last_t;
      spi_gap_v  <= spi_seeded;
      if (!spi_seeded) spi_first <= now;
      if (spi_seeded && spi_q != spi_expect) spi_errs <= spi_errs + 32'd1;
    end
    if (spi_gap_v && spi_gap > spi_maxgap) spi_maxgap <= spi_gap;
  end
end

// ---- UART receiver (8N1, LSB first, 4 clocks per bit) on GPIO14 ----
localparam [7:0] DIV = 8'd4;
reg        rxd = 1'b1;
always @(posedge clk) rxd <= m2[3];
reg  [1:0] rx_state;              // 0 idle, 1 start, 2 data, 3 stop
reg  [7:0] rx_cnt;
reg  [2:0] rx_bit;
reg  [7:0] rx_sh;
reg        rx_v = 1'b0, rx_fe = 1'b0, rx_wait_hi = 1'b0;

always @(posedge clk) begin
  rx_v  <= 1'b0;
  rx_fe <= 1'b0;
  if (!rst_n || clr) begin
    rx_state <= 2'd0;
    rx_wait_hi <= 1'b0;
  end else begin
    case (rx_state)
      // Sample 1 clock after the start edge is seen (0-1 clocks late): 25-50%
      // into each bit, the most margin when the sender is fast, +-2% when slow.
      2'd0: if (rx_wait_hi) rx_wait_hi <= !rxd;          // after a framing error
            else if (!rxd) begin rx_state <= 2'd1; rx_cnt <= DIV / 8'd2 - 8'd2; end
      2'd1: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;
            else if (rxd) rx_state <= 2'd0;              // glitch
            else begin rx_state <= 2'd2; rx_cnt <= DIV - 8'd1; rx_bit <= 3'd0; end
      2'd2: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;
            else begin
              rx_sh  <= {rxd, rx_sh[7:1]};
              rx_cnt <= DIV - 8'd1;
              rx_bit <= rx_bit + 3'd1;
              if (rx_bit == 3'd7) rx_state <= 2'd3;
            end
      2'd3: if (rx_cnt != 8'd0) rx_cnt <= rx_cnt - 8'd1;
            else begin
              rx_state   <= 2'd0;
              rx_v       <= rxd;
              rx_fe      <= !rxd;
              rx_wait_hi <= !rxd;
            end
    endcase
  end
end

reg  [31:0] rx_bytes, rx_errs, rx_first, rx_last_t;
reg  [15:0] rx_ferrs;
reg  [7:0]  rx_last, rx_expect;
reg         rx_seeded;
always @(posedge clk) begin
  if (!rst_n || clr) begin
    rx_seeded <= 1'b0; rx_expect <= 8'd0; rx_bytes <= 32'd0; rx_errs <= 32'd0;
    rx_ferrs <= 16'd0; rx_last <= 8'd0; rx_first <= 32'd0; rx_last_t <= 32'd0;
  end else if (rx_fe) begin
    rx_ferrs <= rx_ferrs + 16'd1;
  end else if (rx_v) begin
    rx_bytes  <= rx_bytes + 32'd1;
    rx_last   <= rx_sh;
    rx_expect <= pat[1] ? lfsr8(rx_sh) : rx_sh + 8'd1;
    rx_seeded <= 1'b1;
    rx_last_t <= now;
    if (!rx_seeded) rx_first <= now;
    if (rx_seeded && rx_sh != rx_expect) rx_errs <= rx_errs + 32'd1;
  end
end

// ---- UART transmitter to the CM0 ----
// Bytes follow the TX pattern; with control bit 1 a new byte starts only
// while the CM0 holds RTS_n low.
reg [9:0]  tx_sh;                 // {stop, data, start}, LSB first
reg [3:0]  tx_bit;
reg [7:0]  tx_cnt;
reg [7:0]  tx_val;
reg [31:0] tx_bytes;
reg        txd;
wire       rts_ok = !ctrl[1] || !m2[2];

always @(posedge clk) begin
  if (!rst_n) begin
    tx_bit <= 4'd0; tx_cnt <= 8'd0; tx_val <= 8'd1; txd <= 1'b1; tx_bytes <= 32'd0;
  end else begin
    if (clr) tx_bytes <= 32'd0;
    if (tx_cnt != 8'd0) begin
      tx_cnt <= tx_cnt - 8'd1;
    end else if (tx_bit != 4'd0) begin
      txd    <= tx_sh[0];
      tx_sh  <= {1'b1, tx_sh[9:1]};
      tx_bit <= tx_bit - 4'd1;
      tx_cnt <= DIV - 8'd1;
    end else if (ctrl[0] && rts_ok) begin
      tx_sh    <= {1'b1, tx_val, 1'b0};
      tx_bit   <= 4'd10;
      tx_val   <= pat[2] ? lfsr8(tx_val) : tx_val + 8'd1;
      if (!clr) tx_bytes <= tx_bytes + 32'd1;
    end else begin
      txd <= 1'b1;
    end
  end
end

assign cm0_out = {ctrl[2], txd};

// ---- Snapshot and register read ----
reg [31:0] s_spi_bytes, s_spi_errs, s_spi_span, s_spi_maxgap;
reg [15:0] s_spi_bursts;
reg [7:0]  s_spi_last;
reg [31:0] s_rx_bytes, s_rx_errs, s_rx_span, s_tx_bytes;
reg [15:0] s_rx_ferrs;
reg [7:0]  s_rx_last;
always @(posedge clk) begin
  if (snap) begin
    s_spi_bytes <= spi_bytes; s_spi_errs <= spi_errs; s_spi_span <= spi_last_t - spi_first;
    s_spi_maxgap <= spi_maxgap; s_spi_bursts <= spi_bursts; s_spi_last <= spi_last;
    s_rx_bytes <= rx_bytes; s_rx_errs <= rx_errs; s_rx_span <= rx_last_t - rx_first;
    s_rx_ferrs <= rx_ferrs; s_rx_last <= rx_last; s_tx_bytes <= tx_bytes;
  end
end

reg [7:0] rd;
always @* begin
  case (reg_addr)
    7'h10: rd = {4'd0, ctrl};
    7'h12: rd = {5'd0, pat};
    7'h13: rd = {3'd0, m2};
    7'h14: rd = REVISION;
    7'h20: rd = s_spi_bytes[7:0];   7'h21: rd = s_spi_bytes[15:8];
    7'h22: rd = s_spi_bytes[23:16]; 7'h23: rd = s_spi_bytes[31:24];
    7'h24: rd = s_spi_errs[7:0];    7'h25: rd = s_spi_errs[15:8];
    7'h26: rd = s_spi_errs[23:16];  7'h27: rd = s_spi_errs[31:24];
    7'h28: rd = s_spi_span[7:0];    7'h29: rd = s_spi_span[15:8];
    7'h2A: rd = s_spi_span[23:16];  7'h2B: rd = s_spi_span[31:24];
    7'h2C: rd = s_spi_maxgap[7:0];  7'h2D: rd = s_spi_maxgap[15:8];
    7'h2E: rd = s_spi_maxgap[23:16];7'h2F: rd = s_spi_maxgap[31:24];
    7'h30: rd = s_spi_bursts[7:0];  7'h31: rd = s_spi_bursts[15:8];
    7'h32: rd = s_spi_last;
    7'h34: rd = s_rx_bytes[7:0];    7'h35: rd = s_rx_bytes[15:8];
    7'h36: rd = s_rx_bytes[23:16];  7'h37: rd = s_rx_bytes[31:24];
    7'h38: rd = s_rx_errs[7:0];     7'h39: rd = s_rx_errs[15:8];
    7'h3A: rd = s_rx_errs[23:16];   7'h3B: rd = s_rx_errs[31:24];
    7'h3C: rd = s_rx_ferrs[7:0];    7'h3D: rd = s_rx_ferrs[15:8];
    7'h3E: rd = s_rx_last;
    7'h40: rd = s_rx_span[7:0];     7'h41: rd = s_rx_span[15:8];
    7'h42: rd = s_rx_span[23:16];   7'h43: rd = s_rx_span[31:24];
    7'h44: rd = s_tx_bytes[7:0];    7'h45: rd = s_tx_bytes[15:8];
    7'h46: rd = s_tx_bytes[23:16];  7'h47: rd = s_tx_bytes[31:24];
    default: rd = 8'd0;
  endcase
end
assign reg_rdata = rd;

/* verilator lint_off UNUSEDSIGNAL */
wire unused = reg_rd | (|hdr_in) | (|rp_in) | (|fifo_tx_data) | fifo_tx_valid | fifo_rx_ready
            | (|reg_wdata[7:4]) | m3[0] | m3[2] | m3[3] | m3[4];
/* verilator lint_on UNUSEDSIGNAL */

endmodule
