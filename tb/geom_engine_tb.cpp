#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vgeom_engine.h"

// Q16.16 conversion helpers
constexpr int32_t to_q16(double val) {
    return static_cast<int32_t>(val * 65536.0);
}

constexpr double from_q16(int32_t val) {
    return static_cast<double>(val) / 65536.0;
}

struct Testbench {
    std::unique_ptr<Vgeom_engine> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    Testbench() {
        top = std::make_unique<Vgeom_engine>();
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
        top->s_stream_vx = 0;
        top->s_stream_vy = 0;
        top->s_stream_vz = 0;
        top->s_stream_color = 0;
        top->s_stream_valid = 0;
        top->m_stream_ready = 1;

        // Initialize 4x4 matrix to zero
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                top->mvp_matrix[r][c] = 0;
            }
        }

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    void set_matrix_identity() {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                top->mvp_matrix[r][c] = (r == c) ? to_q16(1.0) : 0;
            }
        }
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // 1. Load Identity Matrix into Engine
    tb->set_matrix_identity();

    // 2. Drive Vertex Input (X=1.0, Y=2.0, Z=3.0, Color=0xFF0000FF)
    tb->top->s_stream_vx = to_q16(1.0);
    tb->top->s_stream_vy = to_q16(2.0);
    tb->top->s_stream_vz = to_q16(3.0);
    tb->top->s_stream_color = 0xFF0000FF;
    tb->top->s_stream_valid = 1;
    tb->top->m_stream_ready = 1;

    tb->clock_cycle();
    tb->top->s_stream_valid = 0; // Clear valid after 1 cycle

    // 3. Wait through 2 pipeline stages for valid output
    int timeout = 0;
    while (!tb->top->m_stream_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 20 && "Timeout waiting for m_stream_valid");
    }

    std::cout << "Identity Matrix Test Output:" << std::endl;
    std::cout << "  X_clip: " << from_q16(tb->top->m_stream_x_clip) << " (Expected: 1.0)" << std::endl;
    std::cout << "  Y_clip: " << from_q16(tb->top->m_stream_y_clip) << " (Expected: 2.0)" << std::endl;
    std::cout << "  Z_clip: " << from_q16(tb->top->m_stream_z_clip) << " (Expected: 3.0)" << std::endl;
    std::cout << "  W_clip: " << from_q16(tb->top->m_stream_w_clip) << " (Expected: 1.0)" << std::endl;

    assert(tb->top->m_stream_x_clip == to_q16(1.0) && "X clip mismatch");
    assert(tb->top->m_stream_y_clip == to_q16(2.0) && "Y clip mismatch");
    assert(tb->top->m_stream_z_clip == to_q16(3.0) && "Z clip mismatch");
    assert(tb->top->m_stream_w_clip == to_q16(1.0) && "W clip mismatch");
    assert(tb->top->m_stream_color == 0xFF0000FF && "Color mismatch");

    std::cout << "Test 1 passed: Identity Matrix Transformation." << std::endl;

    tb->clock_cycle();

    // 4. Test 2: Scale Matrix by 2x and Translate X by +5.0
    // [ 2  0  0  5 ]
    // [ 0  2  0  0 ]
    // [ 0  0  2  0 ]
    // [ 0  0  0  1 ]
    tb->set_matrix_identity();
    tb->top->mvp_matrix[0][0] = to_q16(2.0);
    tb->top->mvp_matrix[1][1] = to_q16(2.0);
    tb->top->mvp_matrix[2][2] = to_q16(2.0);
    tb->top->mvp_matrix[0][3] = to_q16(5.0); // Translate X

    // Input Vertex (X=-1.0, Y=0.5, Z=0.0)
    // Expected: X = (-1 * 2) + 5 = 3.0, Y = (0.5 * 2) = 1.0, Z = 0.0, W = 1.0
    tb->top->s_stream_vx = to_q16(-1.0);
    tb->top->s_stream_vy = to_q16(0.5);
    tb->top->s_stream_vz = to_q16(0.0);
    tb->top->s_stream_color = 0x00FF00FF;
    tb->top->s_stream_valid = 1;

    tb->clock_cycle();
    tb->top->s_stream_valid = 0;

    timeout = 0;
    while (!tb->top->m_stream_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 20 && "Timeout waiting for m_stream_valid");
    }

    std::cout << "Scale/Translate Matrix Test Output:" << std::endl;
    std::cout << "  X_clip: " << from_q16(tb->top->m_stream_x_clip) << " (Expected: 3.0)" << std::endl;
    std::cout << "  Y_clip: " << from_q16(tb->top->m_stream_y_clip) << " (Expected: 1.0)" << std::endl;
    std::cout << "  Z_clip: " << from_q16(tb->top->m_stream_z_clip) << " (Expected: 0.0)" << std::endl;
    std::cout << "  W_clip: " << from_q16(tb->top->m_stream_w_clip) << " (Expected: 1.0)" << std::endl;

    assert(tb->top->m_stream_x_clip == to_q16(3.0) && "Scale/Trans X mismatch");
    assert(tb->top->m_stream_y_clip == to_q16(1.0) && "Scale/Trans Y mismatch");
    assert(tb->top->m_stream_z_clip == to_q16(0.0) && "Scale/Trans Z mismatch");
    assert(tb->top->m_stream_w_clip == to_q16(1.0) && "Scale/Trans W mismatch");

    std::cout << "Test 2 passed: Scale and Translate Transformation." << std::endl;

    std::cout << "All geometry engine unit tests passed." << std::endl;
    return 0;
}