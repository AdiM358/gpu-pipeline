#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vgpu_top.h"
#include "Vgpu_top___024root.h"
#include "Vgpu_top_gpu_top.h"

constexpr int32_t to_q16(double val) {
    return static_cast<int32_t>(val * 65536.0);
}

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

        top->frag_ready = 1;

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
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // 3 model-space vertices forming a triangle:
    // V0 = (-0.1, -0.1, 0.5) -> Screen (288, 264)
    // V1 = ( 0.0,  0.1, 0.5) -> Screen (320, 216)
    // V2 = ( 0.1, -0.1, 0.5) -> Screen (352, 264)
    Vertex v0 = {to_q16(-0.1), to_q16(-0.1), to_q16(0.5), 0x00FF00FF, {0}};
    Vertex v1 = {to_q16( 0.0), to_q16( 0.1), to_q16(0.5), 0x00FF00FF, {0}};
    Vertex v2 = {to_q16( 0.1), to_q16(-0.1), to_q16(0.5), 0x00FF00FF, {0}};

    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 0,  v0);
    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 32, v1);
    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 64, v2);

    tb->set_identity_matrix();
    tb->axi_lite_write(0x04, Testbench::VRAM_BASE_ADDR);
    tb->axi_lite_write(0x08, 3); // 3 vertices

    tb->axi_lite_write(0x00, 0x00000001); // Trigger start pulse

    std::cout << "Streaming fragments from full GPU pipeline..." << std::endl;
    int fragment_count = 0;
    int cycles = 0;

    while (cycles < 5000) {
        tb->clock_cycle();
        cycles++;
        if (tb->top->rootp->gpu_top->tri_valid) {
            std::cout << "Assembled Tri V0: (" 
                    << (tb->top->rootp->gpu_top->tri_v0_x >> 16) << ", " 
                    << (tb->top->rootp->gpu_top->tri_v0_y >> 16) << ")" << std::endl;
            std::cout << "Assembled Tri V1: (" 
                    << (tb->top->rootp->gpu_top->tri_v1_x >> 16) << ", " 
                    << (tb->top->rootp->gpu_top->tri_v1_y >> 16) << ")" << std::endl;
            std::cout << "Assembled Tri V2: (" 
                    << (tb->top->rootp->gpu_top->tri_v2_x >> 16) << ", " 
                    << (tb->top->rootp->gpu_top->tri_v2_y >> 16) << ")" << std::endl;
        }
        if (tb->top->frag_valid) {
            fragment_count++;
            if (fragment_count <= 5 || fragment_count % 100 == 0) {
                std::cout << "  Fragment #" << fragment_count << ": (" 
                          << tb->top->frag_x << ", " << tb->top->frag_y << ")" << std::endl;
            }
        }
    }

    std::cout << "End-to-End Simulation complete. Total fragments: " << fragment_count << std::endl;
    assert(fragment_count > 0 && "Pipeline produced 0 fragments!");

    std::cout << "Top-level end-to-end integration tests passed successfully" << std::endl;
    return 0;
}