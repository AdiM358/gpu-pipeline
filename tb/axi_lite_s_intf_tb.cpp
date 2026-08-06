#include <iostream>
#include <cassert>
#include <memory>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vaxi_lite_s_intf.h"

struct Testbench {
    std::unique_ptr<Vaxi_lite_s_intf> top;
    std::unique_ptr<VerilatedVcdC> trace;
    uint64_t main_time = 0;

    Testbench() {
        top = std::make_unique<Vaxi_lite_s_intf>();
        Verilated::traceEverOn(true);
        trace = std::make_unique<VerilatedVcdC>();
        top->trace(trace.get(), 99);
        trace->open("waveform.vcd");
    }

    ~Testbench() {
        trace->close();
    }

    void tick() {
        top->S_AXI_CLK = !top->S_AXI_CLK;
        top->eval();
        trace->dump(main_time);
        main_time++;
    }

    void clock_cycle() {
        top->S_AXI_CLK = 0;
        tick();
        top->S_AXI_CLK = 1;
        tick();
    }

    void reset() {
        top->S_AXI_RESETN = 0;
        top->S_AXI_WRITE_ADDR_VALID = 0;
        top->S_AXI_WRITE_DATA_VALID = 0;
        top->S_AXI_BREADY = 0;
        
        for (int i = 0; i < 5; i++) {
            clock_cycle();
        }
        top->S_AXI_RESETN = 1;
        clock_cycle();
        std::cout << "Reset complete." << std::endl;
    }

    // Axi write function that checks for start_pulse during execution
    bool axi_write(uint32_t addr, uint32_t data) {
        bool saw_start_pulse = false;

        std::cout << "Starting write to address 0x" << std::hex << addr 
                  << " with data 0x" << data << std::dec << std::endl;

        top->S_AXI_WRITE_ADDR = addr;
        top->S_AXI_WRITE_DATA = data;
        top->S_AXI_WRITE_ADDR_VALID = 1;
        top->S_AXI_WRITE_DATA_VALID = 1;
        top->S_AXI_BREADY = 1;

        // Advance 1 cycle so slave sees VALID signals and asserts READY
        clock_cycle();

        if (top->start_pulse) saw_start_pulse = true;

        // Deassert VALID signals
        top->S_AXI_WRITE_ADDR_VALID = 0;
        top->S_AXI_WRITE_DATA_VALID = 0;

        // Wait for write response (BVALID)
        int timeout = 0;
        while (!top->S_AXI_BVALID) {
            clock_cycle();
            if (top->start_pulse) saw_start_pulse = true;

            timeout++;
            if (timeout > 20) {
                std::cout << "Error: Timeout waiting for BVALID." << std::endl;
                assert(false);
            }
        }

        // Acknowledge response
        clock_cycle();
        top->S_AXI_BREADY = 0;
        clock_cycle();

        std::cout << "Write completed." << std::endl;
        return saw_start_pulse;
    }
};

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // 1. Test VBUF_BASE_ADDR (Offset 0x04 -> Register 1)
    uint32_t test_vbuf_addr = 0x80000000;
    tb->axi_write(0x04, test_vbuf_addr);
    assert(tb->top->vbuf_base_addr == test_vbuf_addr && "vbuf_base_addr mismatch");
    std::cout << "Test 1 passed: VBUF base address." << std::endl;

    // 2. Test VERTEX_COUNT (Offset 0x08 -> Register 2)
    uint32_t test_vcount = 300;
    tb->axi_write(0x08, test_vcount);
    assert(tb->top->vertex_count == test_vcount && "vertex_count mismatch");
    std::cout << "Test 2 passed: Vertex count." << std::endl;

    // 3. Test MVP Matrix M00 (Offset 0x10 -> Register 4)
    uint32_t test_m00 = 0x0000C000;
    tb->axi_write(0x10, test_m00);
    assert(tb->top->mvp_matrix[0][0] == static_cast<int32_t>(test_m00) && "MVP[0][0] mismatch");
    std::cout << "Test 3 passed: MVP matrix M00." << std::endl;

    // 4. Test MVP Matrix M32 (Offset 0x48 -> Register 18 -> reg_file[14])
    uint32_t test_m32 = 0xffff0000;
    tb->axi_write(0x48, test_m32);
    assert(tb->top->mvp_matrix[3][2] == static_cast<int32_t>(test_m32) && "MVP[3][2] mismatch");
    std::cout << "Test 4 passed: MVP matrix M32." << std::endl;

    // 5. Test Start Pulse Generation (Offset 0x00 -> Register 0, bit 0)
    bool pulse_detected = tb->axi_write(0x00, 0x00000001);
    assert(pulse_detected && "start_pulse was not detected during write to reg 0");
    
    // Check that start_pulse automatically cleared after the transfer
    assert(tb->top->start_pulse == 0 && "start_pulse failed to auto-clear");
    std::cout << "Test 5 passed: Start pulse generation and auto-clearing." << std::endl;

    std::cout << "Axi-lite control register file tests passed." << std::endl;
    return 0;
}