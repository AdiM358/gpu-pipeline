#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vgpu_top.h"

// Fixed point helpers
constexpr int32_t to_q16(double val) {
    return static_cast<int32_t>(val * 65536.0);
}

constexpr double from_q16(int32_t val) {
    return static_cast<double>(val) / 65536.0;
}

// 32-byte VRAM vertex layout
struct Vertex {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t color;
    uint32_t reserved[4];
};

struct Testbench {
    std::unique_ptr<Vgpu_top> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    std::vector<uint32_t> vram;
    static constexpr uint32_t VRAM_BASE_ADDR = 0x80000000;

    uint32_t active_araddr = 0;
    uint8_t active_arlen = 0;
    uint8_t burst_counter = 0;
    bool burst_in_progress = false;

    Testbench() {
        top = std::make_unique<Vgpu_top>();
        Verilated::traceEverOn(true);
        trace = std::make_unique<VerilatedVcdC>();
        top->trace(trace.get(), 99);
        trace->open("waveform.vcd");

        vram.resize(256, 0);
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
        service_axi_memory_requests();
        tick();
    }

    void reset() {
        top->rst_n = 0;

        top->s_axi_awaddr = 0;
        top->s_axi_awvalid = 0;
        top->s_axi_wdata = 0;
        top->s_axi_wstrb = 0xF;
        top->s_axi_wvalid = 0;
        top->s_axi_bready = 0;
        top->s_axi_araddr = 0;
        top->s_axi_arvalid = 0;
        top->s_axi_rready = 0;

        top->m_axi_arready = 0;
        top->m_axi_rvalid = 0;
        top->m_axi_rdata = 0;
        top->m_axi_rresp = 0;
        top->m_axi_rlast = 0;

        top->stream_ready = 1;

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    void axi_lite_write(uint8_t addr, uint32_t value) {
        top->s_axi_awaddr  = addr;
        top->s_axi_awvalid = 1;
        top->s_axi_wdata   = value;
        top->s_axi_wvalid  = 1;
        top->s_axi_bready  = 1;

        bool aw_done = false;
        bool w_done  = false;
        int timeout  = 0;

        while (!aw_done || !w_done) {
            clock_cycle();
            timeout++;
            assert(timeout < 100 && "Timeout waiting for AXI-Lite AWREADY/WREADY");

            if (top->s_axi_awready && top->s_axi_awvalid) {
                top->s_axi_awvalid = 0;
                aw_done = true;
            }
            if (top->s_axi_wready && top->s_axi_wvalid) {
                top->s_axi_wvalid = 0;
                w_done = true;
            }
        }

        timeout = 0;
        while (!top->s_axi_bvalid) {
            clock_cycle();
            timeout++;
            assert(timeout < 100 && "Timeout waiting for AXI-Lite BVALID");
        }

        clock_cycle();
        top->s_axi_bready = 0;
    }

    void set_identity_matrix() {
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                uint8_t addr = 0x10 + (r * 4 + c) * 4;
                uint32_t val = (r == c) ? to_q16(1.0) : 0;
                axi_lite_write(addr, val);
            }
        }
    }

    void load_vertex_into_vram(uint32_t address, const Vertex& v) {
        uint32_t word_offset = (address - VRAM_BASE_ADDR) / 4;

        vram[word_offset + 0] = static_cast<uint32_t>(v.x);
        vram[word_offset + 1] = static_cast<uint32_t>(v.y);
        vram[word_offset + 2] = static_cast<uint32_t>(v.z);
        vram[word_offset + 3] = v.color;
        for (int i = 0; i < 4; i++) {
            vram[word_offset + 4 + i] = v.reserved[i];
        }
    }

    void service_axi_memory_requests() {
        if (top->m_axi_arvalid && !burst_in_progress) {
            top->m_axi_arready = 1;
            active_araddr = top->m_axi_araddr;
            active_arlen = top->m_axi_arlen;
            burst_counter = 0;
            burst_in_progress = true;
        } else {
            top->m_axi_arready = 0;
        }

        if (burst_in_progress && top->m_axi_rready) {
            uint32_t word_offset = (active_araddr - VRAM_BASE_ADDR) / 4 + burst_counter;

            top->m_axi_rdata = vram[word_offset];
            top->m_axi_rvalid = 1;
            top->m_axi_rlast = (burst_counter == active_arlen) ? 1 : 0;

            if (top->m_axi_rvalid && top->m_axi_rready) {
                burst_counter++;
                if (burst_counter > active_arlen) {
                    burst_in_progress = false;
                }
            }
        } else if (!burst_in_progress) {
            top->m_axi_rvalid = 0;
            top->m_axi_rlast = 0;
        }
    }

    void wait_for_stream_valid(int max_cycles = 1000) {
        int timeout = 0;
        while (!top->stream_valid) {
            clock_cycle();
            timeout++;
            if (timeout >= max_cycles) {
                std::cerr << "ERROR: Timeout waiting for stream_valid!" << std::endl;
                assert(false && "Timeout waiting for stream_valid");
            }
        }
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // Model space vertex (0.0, 0.0, 0.5) -> center of screen
    Vertex v0 = {to_q16(0.0), to_q16(0.0), to_q16(0.5), 0xFF0000FF, {0}};

    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 0, v0);

    tb->axi_lite_write(0x04, Testbench::VRAM_BASE_ADDR);
    tb->axi_lite_write(0x08, 1); // 1 vertex
    tb->set_identity_matrix();

    tb->axi_lite_write(0x00, 0x00000001); // Trigger start

    tb->wait_for_stream_valid(1000);

    std::cout << "Screen output for (0.0, 0.0, 0.5):" << std::endl;
    std::cout << "  X_screen: " << from_q16(tb->top->stream_x_screen) << " (Expected: 320.0)" << std::endl;
    std::cout << "  Y_screen: " << from_q16(tb->top->stream_y_screen) << " (Expected: 240.0)" << std::endl;
    std::cout << "  Z_depth:  " << from_q16(tb->top->stream_z_depth)  << " (Expected: 0.5)" << std::endl;

    assert(tb->top->stream_x_screen == to_q16(320.0) && "Center X mismatch");
    assert(tb->top->stream_y_screen == to_q16(240.0) && "Center Y mismatch");
    assert(tb->top->stream_z_depth  == to_q16(0.5)   && "Center Z depth mismatch");

    tb->clock_cycle();

    std::cout << "Top-level integrated pipeline tests passed." << std::endl;
    return 0;
}