// gpu_top system test: renders scenes through the C++ driver over AXI-Lite
// and compares the frame (colour and depth, every pixel) and the performance
// counters against the golden model.
//
// Interim version: the baseline pixel_map writes an external Z-buffer and a
// 32-bit framebuffer, both modelled here.
#include <vector>

#include "Vgpu_top.h"
#include "axi_mem.h"
#include "axil_master.h"
#include "gpu_driver.hpp"
#include "gpu_model.h"
#include "scenes.h"
#include "sim.h"

using Dut = Vgpu_top;

namespace {

constexpr uint32_t kVramBase = 0x8000'0000;
constexpr int kW = 640, kH = 480;

// External Z-buffer (1-cycle read) and framebuffer used by the baseline pixel_map.
struct BackendModel : tb::Agent {
    Dut* d;
    std::vector<uint32_t> z, color;
    uint32_t rd_addr = 0;
    explicit BackendModel(Dut* dut) : d(dut) { clear(); }
    void clear() {
        z.assign(kW * kH, 0xFFFF);
        color.assign(kW * kH, 0);
    }
    void observe() override {
        if (!d->rst_n) return;
        if (d->m_zbuf_wr_en) z.at(d->m_zbuf_wr_addr) = static_cast<uint32_t>(d->m_zbuf_wr_data);
        if (d->m_fb_wr_en) color.at(d->m_fb_wr_addr) = d->m_fb_wr_data;
        rd_addr = d->m_zbuf_rd_addr;
    }
    void drive() override {
        if (rd_addr < z.size()) d->s_zbuf_rd_data = z[rd_addr];
    }
};

struct System {
    tb::Sim<Dut>& sim;
    tb::AxilMaster<Dut> bus;
    tb::AxiMem<Dut> mem;
    BackendModel be;
    gpu::Driver gpu;
    explicit System(tb::Sim<Dut>& s)
        : sim(s), bus(s), mem(s, kVramBase, 1 << 20), be(s.dut.get()), gpu(bus) {
        s.add_agent(&mem);
        s.add_agent(&be);
    }

    // Returns the command's cycle count.
    uint32_t render_and_compare(const scenes::Scene& sc) {
        for (size_t i = 0; i < sc.verts.size(); ++i) {
            const auto& v = sc.verts[i];
            const uint32_t a = kVramBase + 16 * static_cast<uint32_t>(i);
            mem.write32(a, v.x);
            mem.write32(a + 4, v.y);
            mem.write32(a + 8, v.z);
            mem.write32(a + 12, v.color);
        }
        be.clear();
        gpu.set_mvp_q16(sc.mvp);
        gpu.set_vertex_buffer(kVramBase, static_cast<uint32_t>(sc.verts.size()));
        gpu.set_cull(sc.cull_back, sc.front_cw);
        gpu.start(false, true);
        uint32_t status = 0;
        CHECK_MSG(gpu.wait_done(2'000'000, &status), "%s: draw did not complete (STATUS=0x%x)",
                  sc.name.c_str(), status);
        const gpu::PerfCounters pc = gpu.counters();

        model::Config cfg;
        cfg.width = kW;
        cfg.height = kH;
        model::Gpu ref(cfg);
        ref.draw(sc.verts, sc.mvp, sc.cull_back, sc.front_cw);

        int bad_color = 0, bad_depth = 0, first_bad = -1;
        for (int i = 0; i < kW * kH; ++i) {
            const uint32_t c = be.color[i];
            const uint16_t rgb = model::rgb565((c >> 16) & 0xFF, (c >> 8) & 0xFF, c & 0xFF);
            if (rgb != ref.color[i]) ++bad_color, first_bad = first_bad < 0 ? i : first_bad;
            if (be.z[i] != ref.depth[i]) ++bad_depth, first_bad = first_bad < 0 ? i : first_bad;
        }
        CHECK_MSG(bad_color == 0 && bad_depth == 0,
                  "%s: %d colour and %d depth mismatches (first at x=%d y=%d)", sc.name.c_str(), bad_color,
                  bad_depth, first_bad % kW, first_bad / kW);
        CHECK_EQ(pc.verts, ref.counters.verts);
        CHECK_EQ(pc.tri_in, ref.counters.tri_in);
        CHECK_EQ(pc.tri_culled, ref.counters.tri_culled);
        CHECK_EQ(pc.tri_clipped, ref.counters.tri_clipped);
        CHECK_EQ(pc.frag_gen, ref.counters.frag_gen);
        CHECK_EQ(pc.frag_pass, ref.counters.frag_pass);
        std::printf("  %-16s tris %3u (culled %3u, clipped %3u)  frags %6u (pass %6u)  cycles %7u\n",
                    sc.name.c_str(), pc.tri_in, pc.tri_culled, pc.tri_clipped, pc.frag_gen, pc.frag_pass,
                    pc.cycles);
        return pc.cycles;
    }
};

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "gpu_top");
    System sys(sim);
    sys.bus.idle();
    sys.mem.reset_signals();
    sim.reset();
    CHECK_EQ(sys.gpu.id(), static_cast<uint32_t>(GPU_ID_VALUE));

    const double aspect = double(kW) / kH;
    sys.render_and_compare(scenes::single_triangle(aspect));
    sys.render_and_compare(scenes::overlap(aspect));
    sys.render_and_compare(scenes::cube(aspect, 0.7));
    sys.render_and_compare(scenes::clip_cases(aspect));
    for (uint64_t seed = 1; seed <= 4; ++seed) sys.render_and_compare(scenes::random_tris(aspect, seed, 60));

    return sim.finish();
}
