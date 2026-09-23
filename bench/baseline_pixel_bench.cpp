// Throughput of the BASELINE pixel_map (git tag `baseline`) on the same
// full-screen 320x240 fragment stream used in tb/rop_tb.cpp.
// Built by `make baseline-bench` with SCREEN_W=320, SCREEN_H=240.
#include <vector>

#include "Vpixel_map.h"
#include "sim.h"

using Dut = Vpixel_map;

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "baseline_pixel_map");
    Dut& d = *sim.dut;
    constexpr int W = 320, H = 240, N = W * H;
    std::vector<int32_t> z(N, 0x7FFFFFFF);
    d.s_frag_valid = 0;
    sim.reset();

    int sent = 0;
    uint64_t writes = 0;
    const uint64_t t0 = sim.cycle;
    while (sent < N) {
        d.s_frag_x = sent % W;
        d.s_frag_y = sent / W;
        d.s_frag_z = static_cast<int32_t>(sim.rand_u32() & 0xFFFF);
        d.s_frag_color = 0x123456;
        d.s_frag_valid = 1;
        sim.settle();
        const bool hs = d.s_frag_ready;
        if (d.m_zbuf_rd_en) d.s_zbuf_rd_data = z[d.m_zbuf_rd_addr];  // 1-cycle read model
        sim.tick();
        if (d.m_zbuf_wr_en) z[d.m_zbuf_wr_addr] = d.m_zbuf_wr_data;
        writes += d.m_fb_wr_en;
        sent += hs;
    }
    d.s_frag_valid = 0;
    for (int i = 0; i < 4; ++i) {
        sim.tick();
        writes += d.m_fb_wr_en;
    }
    const uint64_t t = sim.cycle - t0;
    CHECK_EQ(writes, static_cast<uint64_t>(N));  // empty Z-buffer: every fragment passes
    std::printf("  baseline pixel_map: %d fragments in %llu cycles (%.2f cycles/fragment)\n", N,
                (unsigned long long)t, double(t) / N);
    return sim.finish();
}
