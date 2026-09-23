`default_nettype none

// Fully pipelined reciprocal: q = floor(2^44 / d) for d in (2^12, 2^31).
//
// Restoring division, one quotient bit per stage (32 stages), so a new
// operand can enter every cycle. The dividend is the constant 2^44, whose
// bits below 2^32 are zero: stage k only shifts the remainder left and
// conditionally subtracts d. The initial remainder is 2^44 >> 32 = 2^12,
// which is < d, so the quotient fits in 32 bits.
//
// An arbitrary payload travels alongside in a shift register with the same
// enable; it has no reset and a single output tap, so it maps to SRLs.
module recip_pipe #(
    parameter int PAY_W = 1
)(
    input  wire              clk,
    input  wire              rst_n,
    input  wire              en,        // advance all stages

    input  wire              in_valid,
    input  wire [30:0]       in_d,      // divisor, must be > 2^12
    input  wire [PAY_W-1:0]  in_pay,

    output logic             out_valid,
    output logic [31:0]      out_q,
    output logic [PAY_W-1:0] out_pay
);

    localparam int N = 32;

    logic          vld [N+1];
    logic [32:0]   rem [N+1];   // remainder before stage k (always < d)
    logic [30:0]   div [N+1];
    logic [31:0]   quo [N+1];
    logic [PAY_W-1:0] pay [N];

    always_comb begin
        vld[0] = in_valid;
        rem[0] = 33'd1 << 12;
        div[0] = in_d;
        quo[0] = '0;
    end

    for (genvar k = 0; k < N; k++) begin : g_stage
        wire [32:0] shifted = {rem[k][31:0], 1'b0};
        wire        ge      = shifted >= {2'b00, div[k]};

        always_ff @(posedge clk or negedge rst_n) begin
            if (!rst_n)  vld[k+1] <= 1'b0;
            else if (en) vld[k+1] <= vld[k];
        end

        always_ff @(posedge clk) begin
            if (en) begin
                rem[k+1] <= ge ? shifted - {2'b00, div[k]} : shifted;
                div[k+1] <= div[k];
                quo[k+1] <= {quo[k][30:0], ge};
            end
        end
    end

    always_ff @(posedge clk) begin
        if (en) begin
            pay[0] <= in_pay;
            for (int k = 1; k < N; k++) pay[k] <= pay[k-1];
        end
    end

    assign out_valid = vld[N];
    assign out_q     = quo[N];
    assign out_pay   = pay[N-1];

endmodule

`default_nettype wire
