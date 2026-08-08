#include <iostream>
#include <cassert>
#include <memory>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vrasterizer.h"

constexpr int32_t to_q16(double val) {
    return static_cast<int32_t>(val * 65536.0);
}

struct Testbench {
    std::unique_ptr<Vrasterizer> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    Testbench() {
        top = std::make_unique<Vrasterizer>();
        Verilated::traceEverOn(true);
        trace = std::make_unique<VerilatedVcdC>();
        top->trace(trace.get(), 99);
        trace->open("waveform.vcd");
    }

    ~Testbench() {
        trace->close();
    }

    void tick() {
        top->clk = !top->clk;
        top->eval();
        trace->dump(main_time);
        main_time++;
    }

    void clock_cycle() {
        top->clk = 0;
        tick();
        top->clk = 1;
        tick();
    }

    void reset() {
        top->rst_n = 0;
        top->s_v0_x = 0; top->s_v0_y = 0; top->s_v0_z = 0;
        top->s_v1_x = 0; top->s_v1_y = 0; top->s_v1_z = 0;
        top->s_v2_x = 0; top->s_v2_y = 0; top->s_v2_z = 0;
        top->s_color = 0;
        top->s_tri_valid = 0;
        top->frag_ready = 1;

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // Define screen space triangle: (10, 10), (20, 10), (10, 20)
    tb->top->s_v0_x = to_q16(10.0); tb->top->s_v0_y = to_q16(10.0); tb->top->s_v0_z = to_q16(0.5);
    tb->top->s_v1_x = to_q16(20.0); tb->top->s_v1_y = to_q16(10.0); tb->top->s_v1_z = to_q16(0.5);
    tb->top->s_v2_x = to_q16(10.0); tb->top->s_v2_y = to_q16(20.0); tb->top->s_v2_z = to_q16(0.5);
    tb->top->s_color = 0x00FF00FF;
    tb->top->s_tri_valid = 1;

    tb->clock_cycle();
    tb->top->s_tri_valid = 0;

    // Collect generated fragments
    int fragment_count = 0;
    while (fragment_count < 100) {
        tb->clock_cycle();
        if (tb->top->frag_valid) {
            fragment_count++;
            std::cout << "Fragment generated: (" << tb->top->frag_x << ", " << tb->top->frag_y << ")" << std::endl;
        }
        // Stop if state machine returns to idle after rasterizing
        if (fragment_count > 0 && !tb->top->frag_valid && tb->top->s_tri_ready) {
            break;
        }
    }

    std::cout << "Total fragments generated for test triangle: " << fragment_count << std::endl;
    assert(fragment_count > 0 && "Rasterizer failed to generate fragments");

    std::cout << "Rasterizer unit tests passed." << std::endl;
    return 0;
}