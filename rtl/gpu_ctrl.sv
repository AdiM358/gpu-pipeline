`default_nettype none

// Command sequencer: CLEAR (optional) -> DRAW (optional) -> drain -> DONE.
//
// A command arriving while busy is ignored. The command is "done" only when
// the vertex fetch has finished *and* every downstream stage reports idle for
// DRAIN_CYCLES consecutive cycles, so DONE means the frame is fully written.
module gpu_ctrl #(
    parameter int DRAIN_CYCLES = 2
)(
    input  wire  clk,
    input  wire  rst_n,

    input  wire  cmd_draw,
    input  wire  cmd_clear,

    output logic start_clear,   // pulse to the clear engine
    input  wire  clear_busy,
    output logic start_draw,    // pulse to vertex fetch
    input  wire  fetch_busy,
    input  wire  pipe_idle,     // every stage after fetch is empty

    output logic busy,
    output logic cmd_start,     // pulse: command accepted (resets perf counters)
    output logic done_set       // pulse: command complete
);

    typedef enum logic [2:0] {
        S_IDLE, S_CLEAR, S_CLEAR_WAIT, S_DRAW, S_DRAIN
    } state_t;

    state_t state;
    logic   draw_pending;
    logic [$clog2(DRAIN_CYCLES + 1)-1:0] idle_run;

    assign busy = (state != S_IDLE);

    always_ff @(posedge clk or negedge rst_n) begin
        if (!rst_n) begin
            state        <= S_IDLE;
            draw_pending <= 1'b0;
            idle_run     <= '0;
            start_clear  <= 1'b0;
            start_draw   <= 1'b0;
            cmd_start    <= 1'b0;
            done_set     <= 1'b0;
        end else begin
            start_clear <= 1'b0;
            start_draw  <= 1'b0;
            cmd_start   <= 1'b0;
            done_set    <= 1'b0;

            case (state)
                S_IDLE: if (cmd_draw || cmd_clear) begin
                    cmd_start    <= 1'b1;
                    draw_pending <= cmd_draw;
                    if (cmd_clear) begin
                        start_clear <= 1'b1;
                        state       <= S_CLEAR;
                    end else begin
                        start_draw <= 1'b1;
                        state      <= S_DRAW;
                    end
                end

                // One cycle for clear_busy to rise after start_clear.
                S_CLEAR: state <= S_CLEAR_WAIT;

                S_CLEAR_WAIT: if (!clear_busy) begin
                    if (draw_pending) begin
                        start_draw <= 1'b1;
                        state      <= S_DRAW;
                    end else begin
                        done_set <= 1'b1;
                        state    <= S_IDLE;
                    end
                end

                // fetch_busy rises the cycle after start_draw; wait for it to
                // fall, then for the rest of the pipeline to empty.
                S_DRAW: begin
                    idle_run <= '0;
                    if (!start_draw && !fetch_busy) state <= S_DRAIN;
                end

                S_DRAIN: begin
                    if (!pipe_idle) idle_run <= '0;
                    else if (idle_run != DRAIN_CYCLES[$bits(idle_run)-1:0]) idle_run <= idle_run + 1'b1;
                    else begin
                        done_set <= 1'b1;
                        state    <= S_IDLE;
                    end
                end

                default: state <= S_IDLE;
            endcase
        end
    end

endmodule

`default_nettype wire
