#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vpixel_map.h"

struct Testbench {
    std::unique_ptr<Vpixel_map> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    static constexpr uint32_t SCREEN_W = 640;
    static constexpr uint32_t SCREEN_H = 480;
    
    // Simulated Z-buffer memory (initialized to max depth)
    std::vector<int32_t> zbuffer;

    // Track framebuffer writes for assertions
    bool wrote_fb = false;
    uint32_t last_fb_addr = 0;
    uint32_t last_fb_color = 0;

    Testbench() {
        top = std::make_unique<Vpixel_map>();
        Verilated::traceEverOn(true);
        trace = std::make_unique<VerilatedVcdC>();
        top->trace(trace.get(), 99);
        trace->open("waveform.vcd");

        // Fill Z-buffer with a very high initial depth value
        zbuffer.resize(SCREEN_W * SCREEN_H, 0x7FFFFFFF);
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

        // Simulate synchronous Z-buffer RAM read
        if (top->m_zbuf_rd_addr < zbuffer.size()) {
            top->s_zbuf_rd_data = zbuffer[top->m_zbuf_rd_addr];
        }

        top->clk = 1;
        tick();

        // Capture Z-buffer and Framebuffer writes on the rising edge
        if (top->m_zbuf_wr_en) {
            if (top->m_zbuf_wr_addr < zbuffer.size()) {
                zbuffer[top->m_zbuf_wr_addr] = top->m_zbuf_wr_data;
            }
        }
        if (top->m_fb_wr_en) {
            wrote_fb = true;
            last_fb_addr = top->m_fb_wr_addr;
            last_fb_color = top->m_fb_wr_data;
        }
    }

    void reset() {
        top->rst_n = 0;
        top->s_frag_x = 0;
        top->s_frag_y = 0;
        top->s_frag_z = 0;
        top->s_frag_color = 0;
        top->s_frag_valid = 0;

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    void send_fragment(int16_t x, int16_t y, int32_t z, uint32_t color) {
        wrote_fb = false; // Reset tracker

        top->s_frag_x = x;
        top->s_frag_y = y;
        top->s_frag_z = z;
        top->s_frag_color = color;
        top->s_frag_valid = 1;

        // Wait for pipeline to accept the fragment
        while (!top->s_frag_ready) {
            clock_cycle();
        }
        clock_cycle(); // Valid triggers on this edge
        top->s_frag_valid = 0;

        // Wait for pipeline to finish processing and return to IDLE
        while (!top->s_frag_ready) {
            clock_cycle();
        }
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    std::cout << "Test 1: Initial fragment (Should write)" << std::endl;
    tb->send_fragment(10, 10, 1000, 0xFF0000FF); // Red
    assert(tb->wrote_fb && "Failed to write initial fragment");
    assert(tb->last_fb_addr == (10 * 640 + 10) && "Wrong pixel address computed");
    assert(tb->zbuffer[10 * 640 + 10] == 1000 && "Z-buffer not updated");

    std::cout << "Test 2: Farther fragment to same pixel (Should fail Z-test)" << std::endl;
    tb->send_fragment(10, 10, 2000, 0x00FF00FF); // Green
    assert(!tb->wrote_fb && "Overwrote pixel with farther depth!");
    assert(tb->zbuffer[10 * 640 + 10] == 1000 && "Z-buffer improperly updated");

    std::cout << "Test 3: Closer fragment to same pixel (Should pass Z-test)" << std::endl;
    tb->send_fragment(10, 10, 500, 0x0000FFFF); // Blue
    assert(tb->wrote_fb && "Failed to overwrite with closer depth");
    assert(tb->last_fb_color == 0x0000FFFF && "Wrong color written");
    assert(tb->zbuffer[10 * 640 + 10] == 500 && "Z-buffer not updated for closer pixel");

    std::cout << "Test 4: Out of bounds fragment (Should be discarded)" << std::endl;
    tb->send_fragment(800, 10, 100, 0xFFFFFFFF); // White (X out of bounds)
    assert(!tb->wrote_fb && "Wrote out of bounds pixel!");

    std::cout << "Pixel map / Z-buffer unit tests passed successfully." << std::endl;
    return 0;
}