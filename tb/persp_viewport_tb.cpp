#include <iostream>
#include <cassert>
#include <memory>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vpersp_viewport.h"

// Convert double to Q16.16 fixed point
constexpr int32_t to_q16(double val) {
    return static_cast<int32_t>(val * 65536.0);
}

// Convert Q16.16 fixed point back to double
constexpr double from_q16(int32_t val) {
    return static_cast<double>(val) / 65536.0;
}

struct Testbench {
    std::unique_ptr<Vpersp_viewport> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    Testbench() {
        top = std::make_unique<Vpersp_viewport>();
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
        top->s_stream_x_clip = 0;
        top->s_stream_y_clip = 0;
        top->s_stream_z_clip = 0;
        top->s_stream_w_clip = 0;
        top->s_stream_color  = 0;
        top->s_stream_valid  = 0;
        top->m_stream_ready  = 1;

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

    // Send clip space vertex (1.0, 1.0, 0.5, W=2.0)
    // Expected screen result for 640x480: X=480.0, Y=120.0, Z=0.25
    tb->top->s_stream_x_clip = to_q16(1.0);
    tb->top->s_stream_y_clip = to_q16(1.0);
    tb->top->s_stream_z_clip = to_q16(0.5);
    tb->top->s_stream_w_clip = to_q16(2.0);
    tb->top->s_stream_color  = 0x12345678;
    tb->top->s_stream_valid  = 1;

    tb->clock_cycle();
    tb->top->s_stream_valid  = 0;

    // Wait for pipeline valid output
    int timeout = 0;
    while (!tb->top->m_stream_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 20 && "Timeout waiting for m_stream_valid");
    }

    std::cout << "Viewport output:" << std::endl;
    std::cout << "  X_screen: " << from_q16(tb->top->m_stream_x_screen) << std::endl;
    std::cout << "  Y_screen: " << from_q16(tb->top->m_stream_y_screen) << std::endl;
    std::cout << "  Z_depth:  " << from_q16(tb->top->m_stream_z_depth)  << std::endl;

    assert(tb->top->m_stream_x_screen == to_q16(480.0) && "X screen mismatch");
    assert(tb->top->m_stream_y_screen == to_q16(120.0) && "Y screen mismatch");
    assert(tb->top->m_stream_z_depth  == to_q16(0.25)  && "Z depth mismatch");
    assert(tb->top->m_stream_color    == 0x12345678    && "Color mismatch");

    std::cout << "Viewport tests passed." << std::endl;
    return 0;
}