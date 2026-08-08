#include <iostream>
#include <cassert>
#include <memory>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vprim_assembly.h"

struct Testbench {
    std::unique_ptr<Vprim_assembly> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    Testbench() {
        top = std::make_unique<Vprim_assembly>();
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
        top->s_v_x = 0;
        top->s_v_y = 0;
        top->s_v_z = 0;
        top->s_v_color = 0;
        top->s_v_valid = 0;
        top->m_tri_ready = 1;

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    void send_vertex(int32_t x, int32_t y, int32_t z, uint32_t color) {
        top->s_v_x = x;
        top->s_v_y = y;
        top->s_v_z = z;
        top->s_v_color = color;
        top->s_v_valid = 1;

        clock_cycle();

        while (!top->s_v_ready) {
            clock_cycle();
        }

        top->s_v_valid = 0;
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // Send 3 vertices to form 1 triangle
    tb->send_vertex(10, 20, 30, 0xFF0000FF); // V0
    tb->send_vertex(40, 50, 60, 0xFF0000FF); // V1
    tb->send_vertex(70, 80, 90, 0xFF0000FF); // V2

    // Verify triangle valid and vertex data
    int timeout = 0;
    while (!tb->top->m_tri_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 20 && "Timeout waiting for m_tri_valid");
    }

    std::cout << "Primitive Assembly Output:" << std::endl;
    std::cout << "  V0: (" << tb->top->m_v0_x << ", " << tb->top->m_v0_y << ", " << tb->top->m_v0_z << ")" << std::endl;
    std::cout << "  V1: (" << tb->top->m_v1_x << ", " << tb->top->m_v1_y << ", " << tb->top->m_v1_z << ")" << std::endl;
    std::cout << "  V2: (" << tb->top->m_v2_x << ", " << tb->top->m_v2_y << ", " << tb->top->m_v2_z << ")" << std::endl;

    assert(tb->top->m_v0_x == 10 && tb->top->m_v0_y == 20 && tb->top->m_v0_z == 30 && "V0 mismatch");
    assert(tb->top->m_v1_x == 40 && tb->top->m_v1_y == 50 && tb->top->m_v1_z == 60 && "V1 mismatch");
    assert(tb->top->m_v2_x == 70 && tb->top->m_v2_y == 80 && tb->top->m_v2_z == 90 && "V2 mismatch");
    assert(tb->top->m_color == 0xFF0000FF && "Color mismatch");

    std::cout << "Primitive assembly unit tests passed." << std::endl;
    return 0;
}