// rop: depth test and framebuffer writes against a sequential reference.
// Fragment streams concentrated on a few pixels make read-after-write hazards
// at distance 1, 2 and 3 frequent; random bubbles check that forwarding tracks
// cycles, not fragments. Clear, readback port, 1 fragment/cycle throughput.
#include <vector>

#include "Vrop.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vrop;

namespace {

constexpr int W = 320, H = 240, NPIX = W * H;

struct Ref {
    std::vector<uint16_t> z = std::vector<uint16_t>(NPIX), c = std::vector<uint16_t>(NPIX);
    void clear(uint16_t color) {
        std::fill(z.begin(), z.end(), 0xFFFF);
        std::fill(c.begin(), c.end(), color);
    }
    bool frag(int x, int y, uint16_t zz, uint8_t r, uint8_t g, uint8_t b) {
        const int a = y * W + x;
        if (zz < z[a]) {
            z[a] = zz;
            c[a] = model::rgb565(r, g, b);
            return true;
        }
        return false;
    }
};

struct PassCounter : tb::Agent {
    Dut& d;
    uint64_t n = 0;
    explicit PassCounter(Dut& dut) : d(dut) {}
    void observe() override {
        if (d.rst_n && d.frag_pass) ++n;
    }
};

// Reads `addrs` from one buffer through the 2-cycle-latency port.
std::vector<uint16_t> readback(tb::Sim<Dut>& sim, const std::vector<int>& addrs, bool depth) {
    Dut& d = *sim.dut;
    std::vector<uint16_t> out;
    d.ext_depth = depth;
    for (size_t i = 0; i < addrs.size() + 2; ++i) {
        if (i < addrs.size()) d.ext_addr = addrs[i];
        sim.tick();
        if (i >= 1) {
            // Data for the address presented two edges ago is now visible
            // (after this tick, rdata holds mem[addr presented at tick i-1]).
            if (i >= 1 && out.size() < addrs.size()) out.push_back(d.ext_data);
        }
    }
    out.resize(addrs.size());
    return out;
}

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "rop");
    Dut& d = *sim.dut;
    PassCounter passes(d);
    sim.add_agent(&passes);
    d.in_valid = 0;
    d.start_clear = 0;
    d.clear_color = 0;
    d.ext_addr = 0;
    d.ext_depth = 0;
    sim.reset();

    Ref ref;
    std::vector<int> all(NPIX);
    for (int i = 0; i < NPIX; ++i) all[i] = i;

    auto clear = [&](uint16_t color) {
        d.clear_color = color;
        d.start_clear = 1;
        sim.tick();
        d.start_clear = 0;
        const uint64_t t0 = sim.cycle;
        while (d.clear_busy) sim.tick();
        ref.clear(color);
        return sim.cycle - t0;
    };
    auto wait_idle = [&]() {
        for (int i = 0; i < 20 && !d.idle; ++i) sim.tick();
        CHECK_EQ(d.idle, 1);
    };
    auto compare_all = [&](const char* what) {
        const auto zc = readback(sim, all, true);
        const auto cc = readback(sim, all, false);
        int bad = 0, first = -1;
        for (int i = 0; i < NPIX; ++i)
            if (zc[i] != ref.z[i] || cc[i] != ref.c[i]) bad++, first = first < 0 ? i : first;
        CHECK_MSG(bad == 0, "%s: %d pixels differ (first %d: rtl z=%04x c=%04x, ref z=%04x c=%04x)", what, bad,
                  first, first >= 0 ? zc[first] : 0, first >= 0 ? cc[first] : 0, first >= 0 ? ref.z[first] : 0,
                  first >= 0 ? ref.c[first] : 0);
    };

    // ---- clear: every pixel, one per cycle
    const uint64_t t_clear = clear(0x1234);
    std::printf("  clear: %llu cycles for %d pixels\n", (unsigned long long)t_clear, NPIX);
    CHECK_MSG(t_clear <= NPIX + 2, "clear took %llu cycles", (unsigned long long)t_clear);
    compare_all("after clear");

    // ---- fragment streams with heavy same-pixel reuse and random bubbles
    uint64_t ref_pass = 0;
    for (int round = 0; round < 40; ++round) {
        const int hot = sim.rand_range(1, 6);  // number of distinct pixels in play
        std::vector<std::pair<int, int>> px;
        for (int i = 0; i < hot; ++i) px.push_back({sim.rand_range(0, W - 1), sim.rand_range(0, H - 1)});
        const int gap_pct = sim.chance(30) ? 0 : sim.rand_range(0, 60);
        for (int n = 0; n < 500; ++n) {
            while (sim.chance(gap_pct)) {
                d.in_valid = 0;
                sim.tick();
            }
            const auto [x, y] = px[sim.rand_range(0, hot - 1)];
            const uint16_t z = static_cast<uint16_t>(sim.chance(20) ? 0x8000 : sim.rand_u32());  // ties too
            const uint8_t r = sim.rand_u32(), g = sim.rand_u32(), b = sim.rand_u32();
            d.in_x = x;
            d.in_y = y;
            d.in_z = z;
            d.in_r = r;
            d.in_g = g;
            d.in_b = b;
            d.in_valid = 1;
            sim.settle();
            CHECK(d.in_ready);
            sim.tick();
            ref_pass += ref.frag(x, y, z, r, g, b);
        }
        d.in_valid = 0;
        wait_idle();
    }
    compare_all("after hazard streams");
    CHECK_EQ(passes.n, ref_pass);

    // ---- full-screen stream at one fragment per cycle, then clear again
    passes.n = 0;
    ref_pass = 0;
    const uint64_t t0 = sim.cycle;
    for (int i = 0; i < NPIX; ++i) {
        const int x = i % W, y = i / W;
        const uint16_t z = static_cast<uint16_t>(sim.rand_u32());
        d.in_x = x;
        d.in_y = y;
        d.in_z = z;
        d.in_r = d.in_g = d.in_b = static_cast<uint8_t>(i);
        d.in_valid = 1;
        sim.tick();
        ref_pass += ref.frag(x, y, z, i, i, i);
    }
    d.in_valid = 0;
    const uint64_t t_stream = sim.cycle - t0;
    wait_idle();
    std::printf("  %d fragments accepted in %llu cycles\n", NPIX, (unsigned long long)t_stream);
    CHECK_EQ(t_stream, static_cast<uint64_t>(NPIX));
    CHECK_EQ(passes.n, ref_pass);
    compare_all("after full-screen stream");

    // Fragments are held off while a clear runs.
    d.clear_color = 0xBEEF;
    d.start_clear = 1;
    sim.tick();
    d.start_clear = 0;
    d.in_valid = 1;
    sim.settle();
    CHECK_EQ(d.in_ready, 0);
    d.in_valid = 0;
    while (d.clear_busy) sim.tick();
    ref.clear(0xBEEF);
    wait_idle();
    compare_all("after second clear");

    return sim.finish();
}
