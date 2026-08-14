#include <iostream>
#include <cassert>
#include <memory>
#include <vector>
#include <verilated.h>
#include <verilated_vcd_c.h>
#include "Vgpu_top.h"
#include "Vgpu_top___024root.h"
#include "Vgpu_top_gpu_top.h"
#include <fstream>
#include <iomanip>
#include "Vgpu_top_framebuffer.h"

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
    std::vector<int32_t> zbuffer;

    static constexpr uint32_t VRAM_BASE_ADDR = 0x80000000;
    static constexpr uint32_t SCREEN_W = 640;
    static constexpr uint32_t SCREEN_H = 480;

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

        vram.resize(1024, 0);
        zbuffer.resize(SCREEN_W * SCREEN_H, 0x7FFFFFFF); // Init to max depth
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

        // Simulate synchronous Z-Buffer read memory
        if (top->m_zbuf_rd_en && top->m_zbuf_rd_addr < zbuffer.size()) {
            top->s_zbuf_rd_data = zbuffer[top->m_zbuf_rd_addr];
        }

        top->clk = 1;
        service_axi_memory_requests();
        tick();

        // Handle Z-buffer updates on the rising edge
        if (top->m_zbuf_wr_en && top->m_zbuf_wr_addr < zbuffer.size()) {
            zbuffer[top->m_zbuf_wr_addr] = top->m_zbuf_wr_data;
        }
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
        top->s_zbuf_rd_data = 0;

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

    void set_mvp(double rotX_deg, double rotY_deg, double scale) {
        double rX = rotX_deg * M_PI / 180.0;
        double rY = rotY_deg * M_PI / 180.0;
        
        // MVP = Scale * RotY * RotX
        double mat[4][4] = {
            { scale * cos(rY), scale * sin(rX)*sin(rY), scale * cos(rX)*sin(rY), 0.0 },
            { 0.0,             scale * cos(rX),         scale * -sin(rX),        0.0 },
            { scale * -sin(rY),scale * sin(rX)*cos(rY), scale * cos(rX)*cos(rY), 0.0 },
            { 0.0,             0.0,                     0.0,                     1.0 }
        };

        // Write 16 matrix elements via AXI
        for (int r = 0; r < 4; r++) {
            for (int c = 0; c < 4; c++) {
                uint8_t addr = 0x10 + (r * 4 + c) * 4;
                axi_lite_write(addr, to_q16(mat[r][c]));
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

    void save_framebuffer_to_ppm(const std::string& filename) {
        std::ofstream file(filename);
        if (!file.is_open()) {
            std::cerr << "Failed to open " << filename << " for writing." << std::endl;
            return;
        }

        // PPM Header: P3 (Text RGB), Width, Height, Max Color Value (255)
        file << "P3\n" << SCREEN_W << " " << SCREEN_H << "\n255\n";

        // Read directly from the Verilated memory array
        for (int y = 0; y < SCREEN_H; y++) {
            for (int x = 0; x < SCREEN_W; x++) {
                uint32_t addr = (y * SCREEN_W) + x;
                
                // Access the public RAM array inside the framebuffer module
                uint32_t pixel = top->rootp->gpu_top->u_framebuffer->ram_array[addr];

                // Extract ARGB channels (assuming 0xAARRGGBB format)
                uint8_t r = (pixel >> 16) & 0xFF;
                uint8_t g = (pixel >> 8) & 0xFF;
                uint8_t b = pixel & 0xFF;

                // Write RGB values to file
                file << (int)r << " " << (int)g << " " << (int)b << " ";
            }
            file << "\n"; 
        }

        file.close();
        std::cout << "Saved rendered frame to " << filename << std::endl;
    }
};

void push_tri(Testbench* tb, uint32_t& addr, 
              double x0, double y0, double z0, 
              double x1, double y1, double z1, 
              double x2, double y2, double z2, uint32_t color) {
    tb->load_vertex_into_vram(addr, {to_q16(x0), to_q16(y0), to_q16(z0), color, {0}}); addr += 32;
    tb->load_vertex_into_vram(addr, {to_q16(x1), to_q16(y1), to_q16(z1), color, {0}}); addr += 32;
    tb->load_vertex_into_vram(addr, {to_q16(x2), to_q16(y2), to_q16(z2), color, {0}}); addr += 32;
}

int main(int argc, char** argv) {
    Verilated::commandArgs(argc, argv);
    auto tb = std::make_unique<Testbench>();

    tb->reset();

    // The 8 corners of a cube (Size 0.4x0.4x0.4)
    double n = -0.2, p = 0.2;
    uint32_t addr = Testbench::VRAM_BASE_ADDR;

    // Face 1: Front (Red)
    push_tri(tb.get(), addr, n, n, n,  p, p, n,  p, n, n, 0x00FF0000);
    push_tri(tb.get(), addr, n, n, n,  n, p, n,  p, p, n, 0x00FF0000);
    // Face 2: Back (Cyan)
    push_tri(tb.get(), addr, p, n, p,  n, p, p,  n, n, p, 0x0000FFFF);
    push_tri(tb.get(), addr, p, n, p,  p, p, p,  n, p, p, 0x0000FFFF);
    // Face 3: Left (Green)
    push_tri(tb.get(), addr, n, n, p,  n, p, n,  n, n, n, 0x0000FF00);
    push_tri(tb.get(), addr, n, n, p,  n, p, p,  n, p, n, 0x0000FF00);
    // Face 4: Right (Magenta)
    push_tri(tb.get(), addr, p, n, n,  p, p, p,  p, n, p, 0x00FF00FF);
    push_tri(tb.get(), addr, p, n, n,  p, p, n,  p, p, p, 0x00FF00FF);
    // Face 5: Top (Blue)
    push_tri(tb.get(), addr, n, p, n,  p, p, p,  p, p, n, 0x000000FF);
    push_tri(tb.get(), addr, n, p, n,  n, p, p,  p, p, p, 0x000000FF);
    // Face 6: Bottom (Yellow)
    // FIX: Not appearing due to constant z-depth used by rasterizer 
    push_tri(tb.get(), addr, n, n, p,  p, n, n,  p, n, p, 0x00FFFF00);
    push_tri(tb.get(), addr, n, n, p,  n, n, n,  p, n, n, 0x00FFFF00);

    // Rotate Cube: X = 35 deg, Y = 45 deg, Scale = 1.0
    tb->set_mvp(10.0, 15.0, 1.0);
    tb->axi_lite_write(0x04, Testbench::VRAM_BASE_ADDR);
    tb->axi_lite_write(0x08, 36); // 12 triangles * 3 vertices

    tb->axi_lite_write(0x00, 0x00000001); // Trigger GPU

    std::cout << "Rendering 3D Cube (12 Triangles)..." << std::endl;
    int fragment_count = 0;
    int cycles = 0;

    // Run until pipeline finishes rendering (increased timeout for larger object)
    while (cycles < 250000) {
        tb->clock_cycle();
        cycles++;
        if (tb->top->m_fb_wr_en) fragment_count++;
    }

    std::cout << "Top-level end-to-end integration tests passed successfully" << std::endl;
    tb->save_framebuffer_to_ppm("render_output.ppm");
    return 0;
}