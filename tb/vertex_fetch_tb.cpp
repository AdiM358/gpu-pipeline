#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vvertex_fetch.h"

// Struct matching 32-byte (8x32-bit word) memory vertex layout
struct Vertex {
    int32_t x;
    int32_t y;
    int32_t z;
    uint32_t color;
    uint32_t reserved[4]; // Remaining 4 words (u, v, normals, etc.)
};

struct Testbench {
    std::unique_ptr<Vvertex_fetch> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    // Simulated VRAM (Direct Memory Model)
    std::vector<uint32_t> vram;
    static constexpr uint32_t VRAM_BASE_ADDR = 0x80000000;

    // Internal memory transfer tracking state
    uint32_t active_araddr = 0;
    uint8_t active_arlen = 0;
    uint8_t burst_counter = 0;
    bool burst_in_progress = false;

    Testbench() {
        top = std::make_unique<Vvertex_fetch>();
        Verilated::traceEverOn(true);
        trace = std::make_unique<VerilatedVcdC>();
        top->trace(trace.get(), 99);
        trace->open("waveform.vcd");
        
        // Allocate 1 KB simulated VRAM space
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

        // Model VRAM reacting to AXI Read Requests
        service_axi_memory_requests();

        tick();
    }

    void reset() {
        top->rst_n = 0;
        top->start_pulse = 0;
        top->vbuf_base_addr = 0;
        top->vertex_count = 0;
        top->m_axi_arready = 0;
        top->m_axi_rvalid = 0;
        top->m_axi_rdata = 0;
        top->m_axi_rlast = 0;
        top->stream_ready = 0;

        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->rst_n = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    // Write a vertex directly into simulated VRAM array
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

    // Behavioral AXI4 Memory Slave responding to vertex_fetch master
    void service_axi_memory_requests() {
        // Handle Read Address Channel (AR) Handshake
        if (top->m_axi_arvalid && !burst_in_progress) {
            top->m_axi_arready = 1;
            active_araddr = top->m_axi_araddr;
            active_arlen = top->m_axi_arlen;
            burst_counter = 0;
            burst_in_progress = true;
        } else {
            top->m_axi_arready = 0;
        }

        // Handle Read Data Channel (R) Streaming
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

    // 1. Setup Test Vertices in VRAM
    Vertex v0 = {0x00010000, 0x00020000, 0x00030000, 0xFF0000FF, {0}}; // X=1.0, Y=2.0, Z=3.0, Red
    Vertex v1 = {static_cast<int32_t>(0xFFFF0000), 0x00008000, 0x00000000, 0x00FF00FF, {0}}; // X=-1.0, Y=0.5, Z=0.0, Green

    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 0, v0);
    tb->load_vertex_into_vram(Testbench::VRAM_BASE_ADDR + 32, v1);

    // 2. Configure Fetch Unit and Trigger Start Pulse
    tb->top->vbuf_base_addr = Testbench::VRAM_BASE_ADDR;
    tb->top->vertex_count = 2;
    tb->top->start_pulse = 1;
    
    tb->clock_cycle();
    tb->top->start_pulse = 0; // Self-clear pulse

    // Assert fetch engine transitioned to busy
    assert(tb->top->busy == 1 && "fetch engine failed to enter busy state");

    // Enable downstream stream receiver (Geometry Engine ready)
    tb->top->stream_ready = 1;

    // 3. Verify Vertex 0 Stream Output
    int timeout = 0;
    while (!tb->top->stream_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 100 && "Timeout waiting for Vertex 0 stream_valid");
    }

    std::cout << "Stream received Vertex 0: X=0x" << std::hex << tb->top->stream_vx 
              << " Y=0x" << tb->top->stream_vy 
              << " Z=0x" << tb->top->stream_vz 
              << " Color=0x" << tb->top->stream_color << std::dec << std::endl;

    assert(tb->top->stream_vx == v0.x && "v0 X mismatch");
    assert(tb->top->stream_vy == v0.y && "v0 Y mismatch");
    assert(tb->top->stream_vz == v0.z && "v0 Z mismatch");
    assert(tb->top->stream_color == v0.color && "v0 Color mismatch");
    std::cout << "Test 1 passed: Vertex 0 fetched correctly." << std::endl;

    tb->clock_cycle(); // Handshake completes

    // 4. Verify Vertex 1 Stream Output
    timeout = 0;
    while (!tb->top->stream_valid) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 100 && "Timeout waiting for Vertex 1 stream_valid");
    }

    std::cout << "Stream received Vertex 1: X=0x" << std::hex << tb->top->stream_vx 
              << " Y=0x" << tb->top->stream_vy 
              << " Z=0x" << tb->top->stream_vz 
              << " Color=0x" << tb->top->stream_color << std::dec << std::endl;

    assert(tb->top->stream_vx == v1.x && "v1 X mismatch");
    assert(tb->top->stream_vy == v1.y && "v1 Y mismatch");
    assert(tb->top->stream_vz == v1.z && "v1 Z mismatch");
    assert(tb->top->stream_color == v1.color && "v1 Color mismatch");
    std::cout << "Test 2 passed: Vertex 1 fetched correctly." << std::endl;

    tb->clock_cycle(); // Handshake completes

    // 5. Verify Done Pulse and Idle Transition
    timeout = 0;
    while (!tb->top->done_pulse) {
        tb->clock_cycle();
        timeout++;
        assert(timeout < 50 && "Timeout waiting for done_pulse");
    }

    assert(tb->top->done_pulse == 1 && "done_pulse missing");
    std::cout << "Test 3 passed: done_pulse asserted." << std::endl;

    tb->clock_cycle();
    assert(tb->top->busy == 0 && "fetch engine failed to return to IDLE");

    std::cout << "Vertex fetch unit tests passed." << std::endl;
    return 0;
}