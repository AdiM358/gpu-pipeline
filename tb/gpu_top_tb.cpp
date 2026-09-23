// gpu_top system test (interim, baseline datapath): drives the GPU purely
// through the C++ driver over AXI4-Lite and checks the command/counter
// plumbing. Pixel-exact checks arrive with the new datapath and golden model.
#include <cmath>
#include <vector>

#include "Vgpu_top.h"
#include "axi_mem.h"
#include "axil_master.h"
#include "gpu_driver.hpp"
#include "sim.h"

using Dut = Vgpu_top;

namespace {

constexpr uint32_t kVramBase = 0x8000'0000;
constexpr uint32_t kW = 640, kH = 480;

// External Z-buffer memory used by the baseline pixel_map (1-cycle read).
struct ZbufModel : tb::Agent {
    Dut* d;
    std::vector<int32_t> z;
    uint64_t fb_writes = 0;
    explicit ZbufModel(Dut* dut) : d(dut), z(kW * kH, 0x7FFFFFFF) {}
    void observe() override {
        if (!d->rst_n) return;  // outputs are random until reset is applied
        if (d->m_zbuf_wr_en && d->m_zbuf_wr_addr < z.size()) z[d->m_zbuf_wr_addr] = d->m_zbuf_wr_data;
        if (d->m_fb_wr_en) {
            ++fb_writes;
            CHECK_MSG(d->m_fb_wr_addr < kW * kH, "framebuffer write address %u out of range",
                      d->m_fb_wr_addr);
        }
        rd_addr = d->m_zbuf_rd_addr;
    }
    void drive() override {
        if (rd_addr < z.size()) d->s_zbuf_rd_data = z[rd_addr];
    }
    uint32_t rd_addr = 0;
};

void push_tri(tb::AxiMem<Dut>& mem, uint32_t& addr, const double v[9], uint32_t color) {
    for (int i = 0; i < 3; ++i) {
        mem.write32(addr + 0, gpu::to_q16(v[3 * i + 0]));
        mem.write32(addr + 4, gpu::to_q16(v[3 * i + 1]));
        mem.write32(addr + 8, gpu::to_q16(v[3 * i + 2]));
        mem.write32(addr + 12, color);
        addr += 32;  // baseline vertex stride
    }
}

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "gpu_top");
    Dut& d = *sim.dut;
    tb::AxilMaster<Dut> bus(sim);
    tb::AxiMem<Dut> mem(sim, kVramBase, 64 * 1024);
    ZbufModel zbuf(&d);
    sim.add_agent(&mem);
    sim.add_agent(&zbuf);
    gpu::Driver gpu(bus);

    bus.idle();
    mem.reset_signals();
    sim.reset();

    CHECK_EQ(gpu.id(), static_cast<uint32_t>(GPU_ID_VALUE));

    // Cube, 12 triangles, same geometry as the original demo.
    const double n = -0.2, p = 0.2;
    const double faces[12][9] = {
        {n, n, n, p, p, n, p, n, n}, {n, n, n, n, p, n, p, p, n},
        {p, n, p, n, p, p, n, n, p}, {p, n, p, p, p, p, n, p, p},
        {n, n, p, n, p, n, n, n, n}, {n, n, p, n, p, p, n, p, n},
        {p, n, n, p, p, p, p, n, p}, {p, n, n, p, p, n, p, p, p},
        {n, p, n, p, p, p, p, p, n}, {n, p, n, n, p, p, p, p, p},
        {n, n, p, p, n, n, p, n, p}, {n, n, p, n, n, n, p, n, n},
    };
    const uint32_t colors[6] = {0xFF0000, 0x00FFFF, 0x00FF00, 0xFF00FF, 0x0000FF, 0xFFFF00};
    uint32_t addr = kVramBase;
    for (int t = 0; t < 12; ++t) push_tri(mem, addr, faces[t], colors[t / 2]);

    const double rx = 10.0 * M_PI / 180, ry = 15.0 * M_PI / 180;
    const double m[16] = {
        std::cos(ry), std::sin(rx) * std::sin(ry), std::cos(rx) * std::sin(ry), 0,
        0, std::cos(rx), -std::sin(rx), 0,
        -std::sin(ry), std::sin(rx) * std::cos(ry), std::cos(rx) * std::cos(ry), 0,
        0, 0, 0, 1,
    };
    gpu.set_mvp(m);
    gpu.set_vertex_buffer(kVramBase, 36);
    gpu.start(false, true);

    uint32_t status = 0;
    CHECK_MSG(gpu.wait_done(100000, &status), "draw did not complete (STATUS=0x%x)", status);
    const gpu::PerfCounters pc = gpu.counters();
    std::printf("  cycles=%u verts=%u tri_in=%u frag_gen=%u frag_pass=%u rast_busy=%u\n",
                pc.cycles, pc.verts, pc.tri_in, pc.frag_gen, pc.frag_pass, pc.rast_busy);

    CHECK_EQ(pc.verts, 36u);
    CHECK_EQ(pc.tri_in, 12u);
    CHECK_MSG(pc.frag_gen > 1000, "only %u fragments generated", pc.frag_gen);
    CHECK_EQ(static_cast<uint64_t>(pc.frag_pass), zbuf.fb_writes);
    CHECK_MSG(pc.frag_pass <= pc.frag_gen, "more depth passes than fragments");

    // DONE must mean the pipeline has fully drained: nothing more is written.
    const uint64_t writes_at_done = zbuf.fb_writes;
    for (int i = 0; i < 2000; ++i) sim.tick();
    CHECK_EQ(zbuf.fb_writes, writes_at_done);
    CHECK_EQ(gpu.status() & (GPU_STATUS_BUSY | GPU_STATUS_DONE), 0u);

    // A zero-vertex draw completes immediately.
    gpu.set_vertex_buffer(kVramBase, 0);
    gpu.start(false, true);
    CHECK(gpu.wait_done(1000));
    CHECK_EQ(gpu.counters().verts, 0u);

    return sim.finish();
}
