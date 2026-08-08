`default_nettype none

module rasterizer #(
    parameter integer DATA_WIDTH = 32,
    parameter integer FRAC_BITS  = 16,
    parameter integer SCREEN_W   = 640,
    parameter integer SCREEN_H   = 480
)(
    input wire clk,
    input wire rst_n,

    // Triangle Input Stream from prim_assembly
    input wire signed [DATA_WIDTH-1:0] s_v0_x, s_v0_y, s_v0_z,
    input wire signed [DATA_WIDTH-1:0] s_v1_x, s_v1_y, s_v1_z,
    input wire signed [DATA_WIDTH-1:0] s_v2_x, s_v2_y, s_v2_z,
    input wire [31:0]                  s_color,
    input wire                         s_tri_valid,
    output logic                       s_tri_ready,

    // Fragment/Pixel Output Stream
    output logic signed [15:0] frag_x,
    output logic signed [15:0] frag_y,
    output logic signed [31:0] frag_z,
    output logic [31:0]        frag_color,
    output logic               frag_valid,
    input wire                 frag_ready
);

    typedef enum logic [1:0] {
        IDLE,
        SETUP,
        RASTER
    } state_t;

    state_t state;

    // Latched triangle attributes
    logic signed [DATA_WIDTH-1:0] v0_x, v0_y, v0_z;
    logic signed [DATA_WIDTH-1:0] v1_x, v1_y;
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [DATA_WIDTH-1:0] v1_z;
    /* verilator lint_on UNUSEDSIGNAL */
    logic signed [DATA_WIDTH-1:0] v2_x, v2_y;
    /* verilator lint_off UNUSEDSIGNAL */
    logic signed [DATA_WIDTH-1:0] v2_z;
    /* verilator lint_on UNUSEDSIGNAL */
    logic [31:0] color_reg;

    // Integer bounding box limits
    logic signed [15:0] min_x, max_x;
    logic signed [15:0] min_y, max_y;
    logic signed [15:0] curr_x, curr_y;

    // Edge deltas (Q16.16 format for 1-pixel step)
    logic signed [63:0] de0_x, de0_y;
    logic signed [63:0] de1_x, de1_y;
    logic signed [63:0] de2_x, de2_y;

    // Incremental edge accumulators (Q16.16 format)
    logic signed [63:0] e0, e1, e2;
    logic signed [63:0] e0_row, e1_row, e2_row;

    logic inside_triangle;

    assign s_tri_ready = (state == IDLE);
    assign frag_x      = curr_x;
    assign frag_y      = curr_y;
    assign frag_z      = v0_z;
    assign frag_color  = color_reg;

    function automatic logic signed [15:0] to_int(input logic signed [DATA_WIDTH-1:0] val);
        return 16'(val >>> FRAC_BITS);
    endfunction

    function automatic logic signed [15:0] min2(input logic signed [15:0] a, b);
        return (a < b) ? a : b;
    endfunction

    function automatic logic signed [15:0] max2(input logic signed [15:0] a, b);
        return (a > b) ? a : b;
    endfunction

    function automatic logic signed [15:0] min3(input logic signed [15:0] a, b, c);
        return min2(min2(a, b), c);
    endfunction

    function automatic logic signed [15:0] max3(input logic signed [15:0] a, b, c);
        return max2(max2(a, b), c);
    endfunction

    // Evaluates cross-product and scales Q32.32 down to Q16.16
    function automatic logic signed [63:0] edge_func(
        input logic signed [DATA_WIDTH-1:0] px, py,
        input logic signed [DATA_WIDTH-1:0] ax, ay,
        input logic signed [DATA_WIDTH-1:0] bx, by
    );
        logic signed [63:0] full_prod;
        full_prod = (64'(px) - 64'(ax)) * (64'(by) - 64'(ay)) - (64'(py) - 64'(ay)) * (64'(bx) - 64'(ax));
        return full_prod >>> FRAC_BITS;
    endfunction

    always_comb begin
        inside_triangle = ((e0 >= 0 && e1 >= 0 && e2 >= 0) || (e0 <= 0 && e1 <= 0 && e2 <= 0));
    end

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state      <= IDLE;
            frag_valid <= 1'b0;
            curr_x     <= '0;
            curr_y     <= '0;
            min_x      <= '0; max_x <= '0;
            min_y      <= '0; max_y <= '0;
            e0 <= '0; e1 <= '0; e2 <= '0;
            e0_row <= '0; e1_row <= '0; e2_row <= '0;
            de0_x <= '0; de0_y <= '0;
            de1_x <= '0; de1_y <= '0;
            de2_x <= '0; de2_y <= '0;
            v0_x <= '0; v0_y <= '0; v0_z <= '0;
            v1_x <= '0; v1_y <= '0; v1_z <= '0;
            v2_x <= '0; v2_y <= '0; v2_z <= '0;
            color_reg <= '0;
        end else begin
            case (state)
                IDLE: begin
                    frag_valid <= 1'b0;
                    if (s_tri_valid && s_tri_ready) begin
                        v0_x <= s_v0_x; v0_y <= s_v0_y; v0_z <= s_v0_z;
                        v1_x <= s_v1_x; v1_y <= s_v1_y; v1_z <= s_v1_z;
                        v2_x <= s_v2_x; v2_y <= s_v2_y; v2_z <= s_v2_z;
                        color_reg <= s_color;
                        state <= SETUP;
                    end
                end

                SETUP: begin
                    logic signed [15:0] bx0, bx1, by0, by1;
                    logic signed [DATA_WIDTH-1:0] start_px, start_py;

                    // Calculate integer bounding box clamped to screen bounds
                    bx0 = max2(16'sd0, min3(to_int(v0_x), to_int(v1_x), to_int(v2_x)));
                    bx1 = min2(16'(SCREEN_W - 1), max3(to_int(v0_x), to_int(v1_x), to_int(v2_x)));
                    by0 = max2(16'sd0, min3(to_int(v0_y), to_int(v1_y), to_int(v2_y)));
                    by1 = min2(16'(SCREEN_H - 1), max3(to_int(v0_y), to_int(v1_y), to_int(v2_y)));

                    min_x  <= bx0; max_x <= bx1;
                    min_y  <= by0; max_y <= by1;
                    curr_x <= bx0; curr_y <= by0;

                    // Per 1-pixel step deltas in Q16.16 format
                    de0_x <= 64'(v1_y) - 64'(v0_y);
                    de0_y <= -(64'(v1_x) - 64'(v0_x));

                    de1_x <= 64'(v2_y) - 64'(v1_y);
                    de1_y <= -(64'(v2_x) - 64'(v1_x));

                    de2_x <= 64'(v0_y) - 64'(v2_y);
                    de2_y <= -(64'(v0_x) - 64'(v2_x));

                    // Initial pixel center (+0.5 offset) at (min_x, min_y) in Q16.16
                    start_px = DATA_WIDTH'((64'(bx0) <<< FRAC_BITS) + (64'd1 <<< (FRAC_BITS - 1)));
                    start_py = DATA_WIDTH'((64'(by0) <<< FRAC_BITS) + (64'd1 <<< (FRAC_BITS - 1)));

                    // Evaluate initial edge functions (returned in Q16.16)
                    e0 <= edge_func(start_px, start_py, v0_x, v0_y, v1_x, v1_y);
                    e1 <= edge_func(start_px, start_py, v1_x, v1_y, v2_x, v2_y);
                    e2 <= edge_func(start_px, start_py, v2_x, v2_y, v0_x, v0_y);

                    e0_row <= edge_func(start_px, start_py, v0_x, v0_y, v1_x, v1_y);
                    e1_row <= edge_func(start_px, start_py, v1_x, v1_y, v2_x, v2_y);
                    e2_row <= edge_func(start_px, start_py, v2_x, v2_y, v0_x, v0_y);

                    state <= RASTER;
                end

                RASTER: begin
                    if (!frag_valid || frag_ready) begin
                        frag_valid <= inside_triangle;

                        if (curr_x < max_x) begin
                            curr_x <= curr_x + 16'sd1;
                            e0 <= e0 + de0_x;
                            e1 <= e1 + de1_x;
                            e2 <= e2 + de2_x;
                        end else begin
                            curr_x <= min_x;
                            if (curr_y < max_y) begin
                                curr_y <= curr_y + 16'sd1;
                                e0 <= e0_row + de0_y;
                                e1 <= e1_row + de1_y;
                                e2 <= e2_row + de2_y;

                                e0_row <= e0_row + de0_y;
                                e1_row <= e1_row + de1_y;
                                e2_row <= e2_row + de2_y;
                            end else begin
                                state      <= IDLE;
                                frag_valid <= 1'b0;
                                curr_y     <= min_y;
                            end
                        end
                    end
                end

                default: state <= IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
