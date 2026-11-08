// user_core.v - fbuf: CM0 -> FPGA framebuffer on a 15 kHz RGB monitor (FW-MAME milestone 3).
//
// The CM0 sends frames over SPI (receiver clocked by its SCLK, as in
// fpga/fastlink). The FPGA keeps two 64 KB frame buffers in the four SPRAMs,
// maps each 8-bit pixel through a 256-entry palette to RGB333, and generates
// FW-TPG's 15 kHz timing: 25 MHz clock, 1589-clock lines (63.56 us), 262-line
// fields (60.05 Hz), RS-170 equalizing and serrated vsync, active-low CSYNC.
// Same header pins and ladder board as FW-TPG (design.toml).
//
// Picture: WIDTH x HEIGHT pixels (WIDTH even, WIDTH x HEIGHT <= 65536, so
// 256x240, 288x224 and 320x200 all fit), each pixel CPP clocks wide (4-8;
// 5 = 200 ns, so 256 pixels fill 1280 of the 1316 active clocks), placed
// XSTART clocks after the start of the active line and YSTART lines below the
// first active line (19; 241 active lines). Outside it the screen is black.
// Byte address y * WIDTH + x in a buffer is pixel (x, y). Geometry is latched
// at the start of each field.
//
// ---- SPI packets (mode 0, MSB first; CS_n = GPIO21 frames each packet) ----
// Bytes are taken in pairs, little-endian (first byte = low byte), so every
// packet has an even length; a trailing odd byte is dropped.
//   byte 0: command, byte 1: argument, bytes 2-3: 16-bit parameter, then data.
//   0x01 PIXELS   param = even byte address in the back buffer; data = pixels
//   0x02 PALETTE  param = first index; data = one 16-bit word per entry,
//                 bits 8:0 = {R[2:0], G[2:0], B[2:0]}
//   0x03 FLIP     show the back buffer from the next field. The swap happens
//                 at the start of vblank (line 260); until then, keep writing
//                 nothing (the back buffer is still the finished frame).
//   0x04 SETREG   argument = register (0 WIDTH, 1 HEIGHT, 2 XSTART,
//                 3 YSTART, 4 CPP, 5 CONTROL), param = value
//   0x05 AUDIO    param ignored; data = 16-bit signed mono samples (16 kHz,
//                 little-endian), pushed byte by byte into the shell's stream
//                 FIFO (FPGA -> MAIN, 4096 bytes), where the FW2 display app
//                 reads them (h\f\x) and plays them. Bytes that find the FIFO
//                 full are dropped and counted.
//   0x06 MENU     param = even byte address in the 2 KB menu memory; data =
//                 bytes. The CM0 publishes its game list here and the FW2
//                 display app reads it through registers 0x39 / 0x40-0x5F.
//                 Layout (the CM0's and the app's): 64 pages of 32 bytes;
//                 page 0 = {count, serial, running game, 0...}, page n =
//                 game n's name (NUL-terminated).
//   others        ignored to the end of the packet
//
// ---- Back to the CM0 ----
// At the start of every vblank and after every flip (one packet if both
// happen together), an 11-byte packet goes out on the UART (GPIO15, 8N1 at
// 6.25 Mbaud = clk/4, 17.6 us):
//   byte 0      1 F P N N N N N : F front buffer, P flip pending, N field count
//   bytes 1-10  0 D D D D D D D : the 64 input bits (registers 0x30-0x37,
//               byte 0x30 bit 0 first), 7 per byte, LSB first, padded with 0
// Only byte 0 has bit 7 set, so a reader can sync on it. A FLIP is done when a
// packet arrives whose F has changed. CTS_n is held low.
//
// ---- Inputs ----
// MAIN (the FW2 display app, over h\f\w) writes the input state to 0x30-0x37;
// the write to 0x37 commits all eight bytes at once, so write all eight in
// one h\f\w. The layout is the display app's (cm0/fb/fwfb.h).
//
// ---- Registers (MAIN, user window) ----
//   0x10      rw  CONTROL: bit 0 test ramp (pixel index = x, ignores the
//                 buffer). Also SETREG 5. Reset 0.
//   0x11      w   command: bit 0 clear counters, bit 1 snapshot, bit 2 flip
//   0x12      r   design revision (1)
//   0x13      r   status: bit 0 front buffer, bit 1 flip pending
//   0x14      r   field counter (live)
//   0x15      r   live CM0 input levels
//   0x18-0x19 rw  WIDTH (reset 256)     0x1A-0x1B rw  HEIGHT (reset 224)
//   0x1C-0x1D rw  XSTART (reset 18)     0x1E-0x1F rw  YSTART (reset 8)
//   0x20      rw  CPP (reset 5, clamped to 4-8)
//   snapshot (command bit 1):
//   0x24-0x27 r   packets received      0x28-0x2B r   words received
//   0x2C-0x2D r   flips done            0x2E      r   ignored packets (unknown command)
//   0x2F      r   errors: bits 3:0 write overruns, bits 7:4 fetch underruns (saturating)
//   0x30-0x37 rw  input state (staged; a write to 0x37 commits). Reads the committed value.
//   0x38      r   input updates (commits, live, wraps)
//   snapshot (command bit 1):
//   0x3A-0x3D r   audio bytes pushed into the stream FIFO
//   0x3E-0x3F r   audio bytes dropped (FIFO full, or a word arriving before
//                 the previous one was pushed)
//   0x39      rw  menu page: writing p copies menu bytes p*32 .. p*32+31 into
//                 0x40-0x5F (done ~20 clocks later)
//   0x40-0x5F r   menu window (the page selected by 0x39)

`include "fw2_profile.vh"

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

localparam [7:0] REVISION = 8'd4;

// ======================================================= video timing (FW-TPG)
localparam [10:0] H_TOTAL   = 11'd1589;
localparam [10:0] HSYNC     = 11'd118;
localparam [10:0] ACT_START = 11'd235;
localparam [10:0] ACT_END   = 11'd1551;
localparam [10:0] HALF      = 11'd795;
localparam [10:0] EQ1_END   = 11'd58;
localparam [10:0] EQ2_END   = 11'd853;
localparam [10:0] VS1_END   = 11'd677;
localparam [10:0] VS2_END   = 11'd1471;
localparam [8:0]  V_TOTAL     = 9'd262;
localparam [8:0]  V_VS_FIRST  = 9'd3;
localparam [8:0]  V_PEQ_FIRST = 9'd6;
localparam [8:0]  V_BLK_FIRST = 9'd9;
localparam [8:0]  V_ACT_FIRST = 9'd19;
localparam [8:0]  V_ACT_LAST  = 9'd259;
localparam [10:0] PREFETCH    = 11'd8;      // fetch starts this many clocks before the first pixel

reg [10:0] hcnt = 11'd0;
reg [8:0]  vcnt = 9'd0;
always @(posedge clk) begin
  if (!rst_n) begin
    hcnt <= 11'd0; vcnt <= 9'd0;
  end else if (hcnt == H_TOTAL - 11'd1) begin
    hcnt <= 11'd0;
    vcnt <= (vcnt == V_TOTAL - 9'd1) ? 9'd0 : vcnt + 9'd1;
  end else begin
    hcnt <= hcnt + 11'd1;
  end
end
wire field_start  = hcnt == 11'd0 && vcnt == 9'd0;
wire vblank_start = hcnt == 11'd0 && vcnt == V_ACT_LAST + 9'd1;

wire vs_line = vcnt >= V_VS_FIRST && vcnt < V_PEQ_FIRST;
wire eq_line = vcnt < V_VS_FIRST || (vcnt >= V_PEQ_FIRST && vcnt < V_BLK_FIRST);
wire second_half = hcnt >= HALF;
reg sync_on;
always @* begin
  if (vs_line)      sync_on = hcnt < VS1_END || (second_half && hcnt < VS2_END);
  else if (eq_line) sync_on = hcnt < EQ1_END || (second_half && hcnt < EQ2_END);
  else              sync_on = hcnt < HSYNC;
end

// ======================================================= registers
reg [15:0] cfg_w, cfg_h, cfg_x, cfg_y;
reg [3:0]  cfg_cpp;
reg        cfg_ramp;
reg        clr = 1'b0, snap = 1'b0, flip_cmd = 1'b0;
always @(posedge clk) begin
  clr      <= reg_wr && reg_addr == 7'h11 && reg_wdata[0];
  snap     <= reg_wr && reg_addr == 7'h11 && reg_wdata[1];
  flip_cmd <= reg_wr && reg_addr == 7'h11 && reg_wdata[2];
end

// SETREG from the SPI parser (below)
reg        sr_v = 1'b0;
reg [7:0]  sr_reg;
reg [15:0] sr_val;

function [3:0] clamp_cpp;
  input [15:0] v;
  clamp_cpp = (v < 16'd4) ? 4'd4 : (v > 16'd8) ? 4'd8 : v[3:0];
endfunction

always @(posedge clk) begin
  if (!rst_n) begin
    cfg_w <= 16'd256; cfg_h <= 16'd224; cfg_x <= 16'd18; cfg_y <= 16'd8;
    cfg_cpp <= 4'd5; cfg_ramp <= 1'b0;
  end else if (sr_v) begin
    case (sr_reg)
      8'd0: cfg_w    <= {sr_val[15:1], 1'b0};
      8'd1: cfg_h    <= sr_val;
      8'd2: cfg_x    <= sr_val;
      8'd3: cfg_y    <= sr_val;
      8'd4: cfg_cpp  <= clamp_cpp(sr_val);
      8'd5: cfg_ramp <= sr_val[0];
      default: ;
    endcase
  end else if (reg_wr) begin
    case (reg_addr)
      7'h10: cfg_ramp      <= reg_wdata[0];
      7'h18: cfg_w[7:0]    <= {reg_wdata[7:1], 1'b0};
      7'h19: cfg_w[15:8]   <= reg_wdata;
      7'h1A: cfg_h[7:0]    <= reg_wdata;
      7'h1B: cfg_h[15:8]   <= reg_wdata;
      7'h1C: cfg_x[7:0]    <= reg_wdata;
      7'h1D: cfg_x[15:8]   <= reg_wdata;
      7'h1E: cfg_y[7:0]    <= reg_wdata;
      7'h1F: cfg_y[15:8]   <= reg_wdata;
      7'h20: cfg_cpp       <= clamp_cpp({8'd0, reg_wdata});
      default: ;
    endcase
  end
end

// Geometry used by the current field, latched at field start. The window is
// clipped to the active area, so a bad XSTART / YSTART can't touch sync.
reg [15:0] f_w = 16'd256;
reg [3:0]  f_cpp = 4'd5;
reg [10:0] f_hx0 = ACT_START + 11'd18;     // first pixel clock
reg [9:0]  f_vy0 = 10'd27, f_vy1 = 10'd251; // first and one-past-last line
reg        f_ramp = 1'b0;
always @(posedge clk) begin
  if (field_start) begin
    f_w    <= cfg_w;
    f_cpp  <= cfg_cpp;
    f_hx0  <= ACT_START + ((cfg_x > 16'd1300) ? 11'd1300 : cfg_x[10:0]);
    f_vy0  <= {1'b0, V_ACT_FIRST} + ((cfg_y > 16'd240) ? 10'd240 : cfg_y[9:0]);
    f_vy1  <= {1'b0, V_ACT_FIRST} + ((cfg_y > 16'd240) ? 10'd240 : cfg_y[9:0])
              + ((cfg_h > 16'd241) ? 10'd241 : cfg_h[9:0]);
    f_ramp <= cfg_ramp;
  end
end

// ======================================================= SPI receiver (SCLK domain)
// 16-bit words, little-endian byte pairs. CS_n high resets the bit counter
// and marks the next word as the first of a packet.
wire sclk   = cm0_in[1];
wire smosi  = cm0_in[0];
wire s_arst = cm0_in[4];

reg [3:0]  sbit = 4'd0;
reg        sfirst = 1'b1;
reg [14:0] ssh = 15'd0;
reg [16:0] shold = 17'd0;            // {first, word}
reg        stog = 1'b0;
always @(posedge sclk or posedge s_arst) begin
  if (s_arst) begin
    sbit   <= 4'd0;
    sfirst <= 1'b1;
  end else begin
    sbit <= sbit + 4'd1;
    if (sbit == 4'd15) sfirst <= 1'b0;
  end
end
always @(posedge sclk) begin
  ssh <= {ssh[13:0], smosi};
  if (sbit == 4'd15) begin
    shold <= {sfirst, ssh[6:0], smosi, ssh[14:7]};   // {first, second byte, first byte}
    stog  <= ~stog;
  end
end

// Word handshake into the design clock: shold is stable for 16 SCLK periods
// and is captured 2-3 clocks after the toggle, so up to ~120 MHz SCLK.
reg t1 = 1'b0, t2 = 1'b0, t3 = 1'b0;
always @(posedge clk) begin t1 <= stog; t2 <= t1; t3 <= t2; end
reg        w_v = 1'b0, w_first = 1'b0;
reg [15:0] w_data = 16'd0;
always @(posedge clk) begin
  w_v <= t2 ^ t3;
  if (t2 ^ t3) {w_first, w_data} <= shold;
end

// ======================================================= packet parser
localparam [7:0] C_PIXELS = 8'h01, C_PALETTE = 8'h02, C_FLIP = 8'h03, C_SETREG = 8'h04, C_AUDIO = 8'h05,
                 C_MENU = 8'h06;
reg        menu_we = 1'b0;            // a menu word to write
reg [9:0]  menu_wa;
reg [15:0] menu_wd;
reg        au_v = 1'b0;               // an audio word for the stream FIFO pusher
reg [15:0] au_w;
reg [1:0]  pst = 2'd0;               // 0 idle, 1 parameter next, 2 data
reg [7:0]  pcmd, parg;
reg [14:0] p_waddr;                  // next word address in the back buffer
reg [7:0]  p_pidx;                   // next palette index
reg        wr_pend = 1'b0;           // a pixel word waiting for an SPRAM slot
reg [14:0] wr_addr;
reg [15:0] wr_data;
reg        pal_we = 1'b0;
reg [7:0]  pal_wa;
reg [8:0]  pal_wd;
reg        flip_spi = 1'b0;
reg        wr_done;                  // from the SPRAM arbiter
reg [31:0] n_packets, n_words;
reg [7:0]  n_ignored;
reg [3:0]  n_overrun;

always @(posedge clk) begin
  sr_v     <= 1'b0;
  pal_we   <= 1'b0;
  flip_spi <= 1'b0;
  au_v     <= 1'b0;
  menu_we  <= 1'b0;
  if (wr_done) wr_pend <= 1'b0;
  if (!rst_n || clr) begin
    n_packets <= 32'd0; n_words <= 32'd0; n_ignored <= 8'd0; n_overrun <= 4'd0;
  end
  if (!rst_n) begin
    pst <= 2'd0; wr_pend <= 1'b0;
  end else if (w_v) begin
    if (!clr) n_words <= n_words + 32'd1;
    if (w_first) begin
      pcmd <= w_data[7:0];
      parg <= w_data[15:8];
      pst  <= 2'd1;
      if (!clr) n_packets <= n_packets + 32'd1;
      if (!clr && w_data[7:0] != C_PIXELS && w_data[7:0] != C_PALETTE && w_data[7:0] != C_FLIP
          && w_data[7:0] != C_SETREG && w_data[7:0] != C_AUDIO && w_data[7:0] != C_MENU
          && n_ignored != 8'hFF)
        n_ignored <= n_ignored + 8'd1;
    end else if (pst == 2'd1) begin
      pst     <= 2'd2;
      p_waddr <= w_data[15:1];
      p_pidx  <= w_data[7:0];
      case (pcmd)
        C_FLIP:   flip_spi <= 1'b1;
        C_SETREG: begin sr_v <= 1'b1; sr_reg <= parg; sr_val <= w_data; end
        default: ;
      endcase
    end else if (pst == 2'd2) begin
      case (pcmd)
        C_PIXELS: begin
          if (wr_pend && !wr_done && n_overrun != 4'hF && !clr) n_overrun <= n_overrun + 4'd1;
          wr_pend <= 1'b1;
          wr_addr <= p_waddr;
          wr_data <= w_data;
          p_waddr <= p_waddr + 15'd1;
        end
        C_PALETTE: begin
          pal_we <= 1'b1;
          pal_wa <= p_pidx;
          pal_wd <= w_data[8:0];
          p_pidx <= p_pidx + 8'd1;
        end
        C_AUDIO: begin
          au_v <= 1'b1;
          au_w <= w_data;
        end
        C_MENU: begin
          menu_we <= 1'b1;
          menu_wa <= p_waddr[9:0];
          menu_wd <= w_data;
          p_waddr <= p_waddr + 15'd1;
        end
        default: ;
      endcase
    end
  end
end

// ======================================================= menu memory (game list)
// 1024 x 16 block RAM written by MENU packets. Writing a page number to 0x39
// copies that page's 16 words into a 32-byte register window (0x40-0x5F), so
// MAIN reads plain registers, never the block RAM's one-clock latency.
reg [15:0] menu_mem [0:1023];
always @(posedge clk) if (menu_we) menu_mem[menu_wa] <= menu_wd;

reg [5:0]   menu_page = 6'd0;
reg [4:0]   mc_cnt = 5'd0;            // words still to copy (16..1), 0 = idle
reg [3:0]   mc_idx;                   // word being read
reg         mc_v = 1'b0;              // menu_q holds word mc_last
reg [3:0]   mc_last;
reg [15:0]  menu_q;
reg [255:0] menu_win = 256'd0;
always @(posedge clk) begin
  menu_q  <= menu_mem[{menu_page, mc_idx}];
  mc_last <= mc_idx;
  mc_v    <= mc_cnt != 5'd0;
  if (reg_wr && reg_addr == 7'h39) begin
    menu_page <= reg_wdata[5:0];
    mc_idx    <= 4'd0;
    mc_cnt    <= 5'd16;
  end else if (mc_cnt != 5'd0) begin
    mc_idx <= mc_idx + 4'd1;
    mc_cnt <= mc_cnt - 5'd1;
  end
  if (mc_v) menu_win[mc_last * 16 +: 16] <= menu_q;
end

// ======================================================= audio -> stream FIFO
// Words arrive at most every 6 clocks (SPI at 66.7 MHz); each is pushed as two
// bytes, low first, one per clock while the FIFO is ready. A byte that finds
// the FIFO full is dropped (the shell also flags RX_OVERRUN).
reg [1:0]  au_left = 2'd0;            // bytes of au_q still to push
reg [15:0] au_q;
reg [31:0] au_bytes;
reg [15:0] au_drops;
wire       au_push = au_left != 2'd0;
always @(posedge clk) begin
  if (!rst_n || clr) begin
    au_bytes <= 32'd0; au_drops <= 16'd0;
  end else begin
    if (au_push && fifo_rx_ready) au_bytes <= au_bytes + 32'd1;
    if (au_push && !fifo_rx_ready && au_drops != 16'hFFFF) au_drops <= au_drops + 16'd1;
  end
  if (!rst_n) begin
    au_left <= 2'd0;
  end else if (au_v) begin                       // a new word replaces anything left
    au_q    <= au_w;
    au_left <= 2'd2;
  end else if (au_push) begin
    au_q    <= {8'd0, au_q[15:8]};
    au_left <= au_left - 2'd1;
  end
end
assign fifo_rx_data  = au_q[7:0];
assign fifo_rx_valid = au_push;

// ======================================================= front / back, flips
// A pending flip happens at the first clock of vertical blank (line 260 to
// line 17; the first picture line's fetch starts on line 19 at the earliest),
// so a FLIP sent during vblank takes effect at once.
reg        front = 1'b0, flip_pend = 1'b0;
reg [7:0]  fields = 8'd0;
reg [15:0] n_flips;
wire       in_vblank = vcnt > V_ACT_LAST || vcnt < V_ACT_FIRST - 9'd1;
wire       flip_now  = flip_pend && in_vblank;
always @(posedge clk) begin
  if (!rst_n) begin
    front <= 1'b0; flip_pend <= 1'b0; fields <= 8'd0;
  end else begin
    if (field_start) fields <= fields + 8'd1;
    if (flip_now) front <= ~front;
    if (flip_spi || flip_cmd) flip_pend <= 1'b1;   // one arriving as a flip happens waits for the next
    else if (flip_now) flip_pend <= 1'b0;
  end
end
always @(posedge clk) begin
  if (!rst_n || clr) n_flips <= 16'd0;
  else if (flip_now) n_flips <= n_flips + 16'd1;
end

// ======================================================= scan-out
// Rows: the line base address (bytes) advances by WIDTH after each row.
reg        row_on = 1'b0;            // this line is inside the picture
reg [15:0] line_base = 16'd0;        // byte address of this row
reg        any_row = 1'b0;           // a row has been shown this field
always @(posedge clk) begin
  if (field_start) begin
    line_base <= 16'd0;
    any_row   <= 1'b0;
    row_on    <= 1'b0;
  end else if (hcnt == 11'd0) begin
    row_on <= {1'b0, vcnt} >= f_vy0 && {1'b0, vcnt} < f_vy1 && vcnt <= V_ACT_LAST;
    if (row_on) begin                // the previous line was a row
      line_base <= line_base + f_w;
      any_row   <= 1'b1;
    end
  end
end

// Pixel counters
reg        px_run = 1'b0;
reg [15:0] px_cnt = 16'd0;
reg [3:0]  cpp_cnt = 4'd0;
wire       px_last_clk = px_run && cpp_cnt == f_cpp - 4'd1;
wire       pop = px_last_clk && px_cnt[0];          // done with the second pixel of a word
always @(posedge clk) begin
  if (!row_on || hcnt >= ACT_END - 11'd1) begin
    px_run <= 1'b0;
  end else if (hcnt == f_hx0 - 11'd1) begin
    px_run  <= f_w != 16'd0;
    px_cnt  <= 16'd0;
    cpp_cnt <= 4'd0;
  end else if (px_run) begin
    if (cpp_cnt == f_cpp - 4'd1) begin
      cpp_cnt <= 4'd0;
      px_cnt  <= px_cnt + 16'd1;
      if (px_cnt == f_w - 16'd1) px_run <= 1'b0;
    end else begin
      cpp_cnt <= cpp_cnt + 4'd1;
    end
  end
end

// Fetch: a 2-word queue fed from the front buffer, read ahead of the pixels.
// Reads have priority over pixel writes; at most one read per 2 * CPP clocks.
reg [14:0] f_addr;
reg [14:0] f_left = 15'd0;           // words still to fetch on this row
reg [1:0]  q_cnt = 2'd0;
reg        inflight = 1'b0;
reg [15:0] q0, q1;
wire       rd_req = f_left != 15'd0 && ({1'b0, q_cnt} + {2'b0, inflight}) < 3'd2;
wire [15:0] sp_q;                    // SPRAM read data (valid the clock after rd_req)
reg [3:0]  n_underrun;

always @(posedge clk) begin
  if (row_on && hcnt == f_hx0 - PREFETCH) begin
    f_addr   <= line_base[15:1];
    f_left   <= f_w[15:1];
    q_cnt    <= 2'd0;
    inflight <= 1'b0;
  end else begin
    inflight <= rd_req;
    if (rd_req) begin
      f_addr <= f_addr + 15'd1;
      f_left <= f_left - 15'd1;
    end
    case ({inflight, pop})
      2'b10: begin if (q_cnt == 2'd0) q0 <= sp_q; else q1 <= sp_q; q_cnt <= q_cnt + 2'd1; end
      2'b01: begin q0 <= q1; if (q_cnt != 2'd0) q_cnt <= q_cnt - 2'd1; end
      2'b11: begin if (q_cnt == 2'd1) q0 <= sp_q; else begin q0 <= q1; q1 <= sp_q; end end
      default: ;
    endcase
  end
end
always @(posedge clk) begin
  if (!rst_n || clr) n_underrun <= 4'd0;
  else if (pop && q_cnt == 2'd0 && !inflight && n_underrun != 4'hF) n_underrun <= n_underrun + 4'd1;
end

// ======================================================= SPRAM: 4 x 16K x 16
// Word address {buffer, 15-bit word}: bits 15:14 pick the block.
wire        do_wr  = wr_pend && !rd_req;
wire [15:0] sp_a   = rd_req ? {front, f_addr} : {~front, wr_addr};
always @* wr_done = do_wr;
reg  [1:0]  rd_blk = 2'd0;
always @(posedge clk) rd_blk <= sp_a[15:14];
wire [63:0] sp_do;
genvar gi;
generate
  for (gi = 0; gi < 4; gi = gi + 1) begin : g_spram
    SB_SPRAM256KA u_spram (
      .ADDRESS(sp_a[13:0]), .DATAIN(wr_data), .MASKWREN(4'b1111),
      .WREN(do_wr && sp_a[15:14] == gi), .CHIPSELECT(1'b1), .CLOCK(clk),
      .STANDBY(1'b0), .SLEEP(1'b0), .POWEROFF(1'b1), .DATAOUT(sp_do[gi * 16 +: 16])
    );
  end
endgenerate
assign sp_q = rd_blk[1] ? (rd_blk[0] ? sp_do[63:48] : sp_do[47:32])
                        : (rd_blk[0] ? sp_do[31:16] : sp_do[15:0]);

// ======================================================= palette (block RAM)
reg [8:0] pal [0:255];
integer pi;
initial begin
  // default: 3-3-2, index = RRRGGGBB
  for (pi = 0; pi < 256; pi = pi + 1)
    pal[pi] = {pi[7:5], pi[4:2], pi[1:0], pi[1]};
end
always @(posedge clk) if (pal_we) pal[pal_wa] <= pal_wd;

// ======================================================= pixel pipeline
// stage 1: pixel index; stage 2: palette read; stage 3: output pins.
reg [7:0] s1_idx = 8'd0;
reg       s1_vis = 1'b0, s1_sync = 1'b0;
always @(posedge clk) begin
  s1_idx  <= f_ramp ? px_cnt[7:0] : (px_cnt[0] ? q0[15:8] : q0[7:0]);
  s1_vis  <= px_run;
  s1_sync <= sync_on;
end
reg [8:0] s2_rgb = 9'd0;
reg       s2_vis = 1'b0, s2_sync = 1'b0;
always @(posedge clk) begin
  s2_rgb  <= pal[s1_idx];
  s2_vis  <= s1_vis;
  s2_sync <= s1_sync;
end
wire [2:0] r = s2_vis ? s2_rgb[8:6] : 3'd0;
wire [2:0] g = s2_vis ? s2_rgb[5:3] : 3'd0;
wire [2:0] b = s2_vis ? s2_rgb[2:0] : 3'd0;

reg [9:0] out_q = 10'b1 << `FW2_SIG_GPIO27;
always @(posedge clk) begin
  if (!rst_n) begin
    out_q <= 10'b1 << `FW2_SIG_GPIO27;
  end else begin
    out_q[`FW2_SIG_UART_TX]  <= b[2];
    out_q[`FW2_SIG_UART_RX]  <= b[1];
    out_q[`FW2_SIG_UART_CTS] <= b[0];
    out_q[`FW2_SIG_UART_RTS] <= g[2];
    out_q[`FW2_SIG_SPI_MISO] <= g[1];
    out_q[`FW2_SIG_SPI_CS]   <= g[0];
    out_q[`FW2_SIG_SPI_SCLK] <= r[2];
    out_q[`FW2_SIG_SPI_MOSI] <= r[1];
    out_q[`FW2_SIG_GPIO26]   <= r[0];
    out_q[`FW2_SIG_GPIO27]   <= ~s2_sync;
  end
end
assign hdr_out = out_q;
assign hdr_oe  = 10'h3FF;

// ======================================================= inputs from MAIN
reg [63:0] in_stage = 64'd0, inputs = 64'd0;
reg [7:0]  in_updates = 8'd0;
always @(posedge clk) begin
  if (!rst_n) begin
    in_stage <= 64'd0; inputs <= 64'd0; in_updates <= 8'd0;
  end else if (reg_wr && reg_addr[6:3] == 4'h6) begin            // 0x30-0x37
    in_stage[reg_addr[2:0] * 8 +: 8] <= reg_wdata;
    if (reg_addr[2:0] == 3'd7) begin
      inputs     <= {reg_wdata, in_stage[55:0]};
      in_updates <= in_updates + 8'd1;
    end
  end
end

// ======================================================= UART packets to the CM0
// 11 bytes {status, 10 x 7 input bits} at every vblank start and after every
// flip (one packet if both happen together).
localparam [3:0] DIV = 4'd4;         // 6.25 Mbaud
reg [9:0]  tx_sh = 10'h3FF;
reg [3:0]  tx_bit = 4'd0, tx_cnt = 4'd0;
reg [3:0]  tx_byte = 4'd0;           // bytes still to send after the current one
reg [69:0] tx_data = 70'd0;          // input bits not yet sent
reg        tx_req = 1'b0;
reg        txd = 1'b1;
always @(posedge clk) begin
  if (!rst_n) begin
    tx_bit <= 4'd0; tx_cnt <= 4'd0; tx_byte <= 4'd0; txd <= 1'b1; tx_req <= 1'b0;
  end else begin
    if (vblank_start || flip_now) tx_req <= 1'b1;   // sent from the next clock: front is updated
    if (tx_bit == 4'd0 && tx_cnt == 4'd0 && tx_byte != 4'd0) begin
      tx_sh   <= {1'b1, 1'b0, tx_data[6:0], 1'b0};  // next input byte
      tx_data <= {7'd0, tx_data[69:7]};
      tx_bit  <= 4'd10;
      tx_byte <= tx_byte - 4'd1;
    end else if (tx_req && tx_bit == 4'd0 && tx_cnt == 4'd0) begin
      tx_sh   <= {1'b1, 1'b1, front, flip_pend, fields[4:0], 1'b0};
      tx_data <= {6'd0, inputs};
      tx_bit  <= 4'd10;
      tx_byte <= 4'd10;
      if (!(vblank_start || flip_now)) tx_req <= 1'b0;
    end else if (tx_cnt != 4'd0) begin
      tx_cnt <= tx_cnt - 4'd1;
    end else if (tx_bit != 4'd0) begin   // start, 8 data, stop: each DIV clocks
      txd    <= tx_sh[0];
      tx_sh  <= {1'b1, tx_sh[9:1]};
      tx_bit <= tx_bit - 4'd1;
      tx_cnt <= DIV - 4'd1;
    end else begin
      txd <= 1'b1;
    end
  end
end
assign cm0_out = {1'b0, txd};        // CTS_n low, UART TX

// ======================================================= snapshot and register read
reg [31:0] s_packets, s_words;
reg [15:0] s_flips;
reg [7:0]  s_ignored, s_errs;
reg [31:0] s_au_bytes;
reg [15:0] s_au_drops;
always @(posedge clk) begin
  if (snap) begin
    s_packets <= n_packets; s_words <= n_words; s_flips <= n_flips;
    s_ignored <= n_ignored; s_errs <= {n_underrun, n_overrun};
    s_au_bytes <= au_bytes; s_au_drops <= au_drops;
  end
end

reg [7:0] rd;
always @* begin
  case (reg_addr)
    7'h10: rd = {7'd0, cfg_ramp};
    7'h12: rd = REVISION;
    7'h13: rd = {6'd0, flip_pend, front};
    7'h14: rd = fields;
    7'h15: rd = {3'd0, cm0_in};
    7'h18: rd = cfg_w[7:0];   7'h19: rd = cfg_w[15:8];
    7'h1A: rd = cfg_h[7:0];   7'h1B: rd = cfg_h[15:8];
    7'h1C: rd = cfg_x[7:0];   7'h1D: rd = cfg_x[15:8];
    7'h1E: rd = cfg_y[7:0];   7'h1F: rd = cfg_y[15:8];
    7'h20: rd = {4'd0, cfg_cpp};
    7'h24: rd = s_packets[7:0];  7'h25: rd = s_packets[15:8];
    7'h26: rd = s_packets[23:16]; 7'h27: rd = s_packets[31:24];
    7'h28: rd = s_words[7:0];    7'h29: rd = s_words[15:8];
    7'h2A: rd = s_words[23:16];  7'h2B: rd = s_words[31:24];
    7'h2C: rd = s_flips[7:0];    7'h2D: rd = s_flips[15:8];
    7'h2E: rd = s_ignored;
    7'h2F: rd = s_errs;
    7'h30: rd = inputs[7:0];   7'h31: rd = inputs[15:8];
    7'h32: rd = inputs[23:16]; 7'h33: rd = inputs[31:24];
    7'h34: rd = inputs[39:32]; 7'h35: rd = inputs[47:40];
    7'h36: rd = inputs[55:48]; 7'h37: rd = inputs[63:56];
    7'h38: rd = in_updates;
    7'h3A: rd = s_au_bytes[7:0];   7'h3B: rd = s_au_bytes[15:8];
    7'h3C: rd = s_au_bytes[23:16]; 7'h3D: rd = s_au_bytes[31:24];
    7'h3E: rd = s_au_drops[7:0];   7'h3F: rd = s_au_drops[15:8];
    7'h39: rd = {2'd0, menu_page};
    default:
      if (reg_addr[6:5] == 2'b10) rd = menu_win[reg_addr[4:0] * 8 +: 8];   // 0x40-0x5F
      else rd = 8'd0;
  endcase
end
assign reg_rdata = rd;

assign rp_out        = 10'd0;
assign fifo_tx_ready = 1'b0;

/* verilator lint_off UNUSEDSIGNAL */
wire unused = reg_rd | (|hdr_in) | (|rp_in) | (|fifo_tx_data) | fifo_tx_valid
            | any_row | cm0_in[2] | cm0_in[3];
/* verilator lint_on UNUSEDSIGNAL */

endmodule
