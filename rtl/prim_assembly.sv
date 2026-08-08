`default_nettype none

module prim_assembly #(
    parameter integer DATA_WIDTH = 32
)(
    input wire clk,
    input wire rst_n,

    // Input vertex stream from persp_viewport
    input wire signed [DATA_WIDTH-1:0] s_v_x,
    input wire signed [DATA_WIDTH-1:0] s_v_y,
    input wire signed [DATA_WIDTH-1:0] s_v_z,
    input wire [31:0]                  s_v_color,
    input wire                         s_v_valid,
    output logic                       s_v_ready,

    // Output triangle stream to rasterizer
    output logic signed [DATA_WIDTH-1:0] m_v0_x, m_v0_y, m_v0_z,
    output logic signed [DATA_WIDTH-1:0] m_v1_x, m_v1_y, m_v1_z,
    output logic signed [DATA_WIDTH-1:0] m_v2_x, m_v2_y, m_v2_z,
    output logic [31:0]                  m_color,
    output logic                         m_tri_valid,
    input wire                           m_tri_ready
);

    // Vertex accumulation registers
    logic signed [DATA_WIDTH-1:0] v0_x_reg, v0_y_reg, v0_z_reg;
    logic signed [DATA_WIDTH-1:0] v1_x_reg, v1_y_reg, v1_z_reg;
    logic signed [DATA_WIDTH-1:0] v2_x_reg, v2_y_reg, v2_z_reg;
    logic [31:0] color_reg;

    // Counter for tracking 3 vertices per triangle
    logic [1:0] v_count;
    logic tri_valid_reg;

    // Ready when output is ready or no valid triangle is pending
    assign s_v_ready = !tri_valid_reg || m_tri_ready;

    // Output assignments
    assign m_v0_x = v0_x_reg;
    assign m_v0_y = v0_y_reg;
    assign m_v0_z = v0_z_reg;
    assign m_v1_x = v1_x_reg;
    assign m_v1_y = v1_y_reg;
    assign m_v1_z = v1_z_reg;
    assign m_v2_x = v2_x_reg;
    assign m_v2_y = v2_y_reg;
    assign m_v2_z = v2_z_reg;
    assign m_color = color_reg;
    assign m_tri_valid = tri_valid_reg;

    // To-do: add backface culling maybe?

    // Accumulate vertices and generate triangle valid
    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            v_count       <= 2'd0;
            tri_valid_reg <= 1'b0;
            v0_x_reg      <= '0;
            v0_y_reg      <= '0;
            v0_z_reg      <= '0;
            v1_x_reg      <= '0;
            v1_y_reg      <= '0;
            v1_z_reg      <= '0;
            v2_x_reg      <= '0;
            v2_y_reg      <= '0;
            v2_z_reg      <= '0;
            color_reg     <= '0;
        end else begin
            // Clear valid flag when downstream handshakes
            if (m_tri_valid && m_tri_ready) begin
                tri_valid_reg <= 1'b0;
            end

            // Process incoming vertex when ready
            if (s_v_valid && s_v_ready) begin
                case (v_count)
                    2'd0: begin
                        v0_x_reg  <= s_v_x;
                        v0_y_reg  <= s_v_y;
                        v0_z_reg  <= s_v_z;
                        color_reg <= s_v_color;
                        v_count   <= 2'd1;
                    end
                    2'd1: begin
                        v1_x_reg <= s_v_x;
                        v1_y_reg <= s_v_y;
                        v1_z_reg <= s_v_z;
                        v_count  <= 2'd2;
                    end
                    2'd2: begin
                        v2_x_reg      <= s_v_x;
                        v2_y_reg      <= s_v_y;
                        v2_z_reg      <= s_v_z;
                        v_count       <= 2'd0;
                        tri_valid_reg <= 1'b1;
                    end
                    default: v_count <= 2'd0;
                endcase
            end
        end
    end

endmodule

`default_nettype wire
