// rasterizer: fragment stream identical (order and values) to
// model::rasterize for random triangles under random output back-pressure;
// fill rule: a jittered mesh covers every interior pixel exactly once;
// throughput report. Built for several SPAN values (see Makefile).
#include <algorithm>
#include <deque>
#include <map>

#include "Vrasterizer.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vrasterizer;
using model::ScreenVtx;

namespace {

const model::Config kCfg;
uint64_t mask_bits(int64_t v, int bits) { return static_cast<uint64_t>(v) & ((1ULL << bits) - 1); }

struct Harness : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    std::vector<model::TriSetup> to_send;
    std::deque<model::Fragment> expected;
    std::map<uint32_t, int> coverage;  // pixel -> times written (fill-rule test)
    size_t sent = 0;
    uint64_t received = 0, busy_cycles = 0;
    int valid_pct = 100, ready_pct = 100;
    bool last_hs = false;
    explicit Harness(tb::Sim<Dut>& s) : sim(s), d(*s.dut) {}

    void observe() override {
        last_hs = d.in_valid && d.in_ready;
        if (!d.rst_n) return;
        if (d.busy) ++busy_cycles;
        if (last_hs) {
            std::vector<model::Fragment> f;
            model::rasterize(to_send[sent++], f);
            expected.insert(expected.end(), f.begin(), f.end());
        }
        if (d.frag_valid && d.frag_ready) {
            const model::Fragment got{static_cast<uint16_t>(d.frag_x), static_cast<uint16_t>(d.frag_y), d.frag_z,
                                      d.frag_r, d.frag_g, d.frag_b};
            ++coverage[got.y * 1024u + got.x];
            if (expected.empty()) {
                CHECK_MSG(false, "unexpected fragment (%u,%u)", got.x, got.y);
            } else {
                const model::Fragment& e = expected.front();
                CHECK_MSG(got == e, "fragment %llu: got (%u,%u) z=%u rgb=%u,%u,%u expected (%u,%u) z=%u rgb=%u,%u,%u",
                          (unsigned long long)received, got.x, got.y, got.z, got.r, got.g, got.b, e.x, e.y, e.z,
                          e.r, e.g, e.b);
                expected.pop_front();
            }
            ++received;
        }
    }

    void drive() override {
        if (!(d.in_valid && !last_hs)) {
            d.in_valid = sent < to_send.size() && sim.chance(valid_pct);
            if (sent < to_send.size()) load(to_send[sent]);
        }
        d.frag_ready = sim.chance(ready_pct);
    }

    void load(const model::TriSetup& t) {
        d.in_px0 = t.px0;
        d.in_px1 = t.px1;
        d.in_py0 = t.py0;
        d.in_py1 = t.py1;
        for (int e = 0; e < 3; ++e) {
            d.in_e0[e] = mask_bits(t.e0[e], 34);
            d.in_ex[e] = static_cast<uint32_t>(mask_bits(t.ex[e], 21));
            d.in_ey[e] = static_cast<uint32_t>(mask_bits(t.ey[e], 21));
        }
        for (int i = 0; i < 4; ++i) {
            d.in_a0[i] = mask_bits(t.a0[i], 38);
            d.in_ax[i] = mask_bits(t.ax[i], 38);
            d.in_ay[i] = mask_bits(t.ay[i], 38);
            d.in_amin[i] = t.amin[i];
            d.in_amax[i] = t.amax[i];
        }
    }
};

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "rasterizer");
    Dut& d = *sim.dut;
    Harness h(sim);
    sim.add_agent(&h);
    d.in_valid = 0;
    d.frag_ready = 1;
    sim.reset();
    CHECK_EQ(d.idle, 1);

    auto rnd_vtx = [&](int lo_x, int hi_x, int lo_y, int hi_y) {
        return ScreenVtx{static_cast<int16_t>(sim.rand_range(lo_x, hi_x)),
                         static_cast<int16_t>(sim.rand_range(lo_y, hi_y)), static_cast<uint16_t>(sim.rand_u32()),
                         sim.rand_u32() & 0xFFFFFF, 0};
    };
    auto add = [&](const ScreenVtx& a, const ScreenVtx& b, const ScreenVtx& c) {
        const ScreenVtx v[3] = {a, b, c};
        model::TriSetup t;
        if (model::setup(v, false, false, kCfg, &t) == model::TriFate::Raster) h.to_send.push_back(t);
    };
    auto run = [&]() {
        const uint64_t t0 = sim.cycle;
        while (h.sent < h.to_send.size() || !d.idle) {
            sim.tick();
            if (sim.cycle - t0 > 20'000'000) FATAL("rasterizer stalled");
        }
        CHECK_MSG(h.expected.empty(), "%zu fragments missing", h.expected.size());
        return sim.cycle - t0;
    };

    // ---- throughput on a fixed workload: 200 medium triangles, no back-pressure.
    // A dedicated RNG keeps this workload identical for every seed and SPAN.
    {
        std::mt19937_64 fixed(12345);
        auto r = [&](int lo, int hi) { return std::uniform_int_distribution<int>(lo, hi)(fixed); };
        for (int i = 0; i < 200; ++i) {
            ScreenVtx v[3];
            v[0] = {static_cast<int16_t>(r(0, 16 * 320 - 1)), static_cast<int16_t>(r(0, 16 * 240 - 1)),
                    static_cast<uint16_t>(r(0, 65535)), static_cast<uint32_t>(r(0, 0xFFFFFF)), 0};
            for (int k = 1; k < 3; ++k)
                v[k] = {static_cast<int16_t>(v[0].sx + r(-16 * 30, 16 * 30)),
                        static_cast<int16_t>(v[0].sy + r(-16 * 30, 16 * 30)), static_cast<uint16_t>(r(0, 65535)),
                        static_cast<uint32_t>(r(0, 0xFFFFFF)), 0};
            add(v[0], v[1], v[2]);
        }
    }
    const uint64_t before = h.received;
    h.busy_cycles = 0;
    const uint64_t t = run();
    const uint64_t frags = h.received - before;
    std::printf("  fixed workload (%zu triangles): %llu fragments in %llu cycles, %.3f fragments/cycle\n",
                h.to_send.size(), (unsigned long long)frags, (unsigned long long)t, double(frags) / t);

    // ---- random triangles of all sizes, random handshakes
    h.to_send.clear();
    h.sent = 0;
    for (int i = 0; i < 400; ++i) {
        switch (sim.rand_range(0, 2)) {
            case 0: add(rnd_vtx(-800, 6000, -800, 4600), rnd_vtx(-800, 6000, -800, 4600),
                        rnd_vtx(-800, 6000, -800, 4600)); break;
            case 1: {
                const ScreenVtx a = rnd_vtx(0, 5100, 0, 3800);
                auto near = [&]() { return rnd_vtx(a.sx - 60, a.sx + 60, a.sy - 60, a.sy + 60); };
                add(a, near(), near());
                break;
            }
            default: add(rnd_vtx(-16384, 16383, -16384, 16383), rnd_vtx(-16384, 16383, -16384, 16383),
                         rnd_vtx(-16384, 16383, -16384, 16383));
        }
    }
    h.valid_pct = 60;
    h.ready_pct = 50;
    run();

    // ---- fill rule: jittered mesh, vertices often exactly on pixel centres;
    //      every covered pixel must be written exactly once, no interior holes.
    h.to_send.clear();
    h.sent = 0;
    h.coverage.clear();
    h.valid_pct = h.ready_pct = 100;
    constexpr int N = 10, CELL = 16 * 12, X0 = 16 * 40 + 3, Y0 = 16 * 30 + 3;
    ScreenVtx grid[N + 1][N + 1];
    for (int j = 0; j <= N; ++j)
        for (int i = 0; i <= N; ++i) {
            int x = X0 + i * CELL, y = Y0 + j * CELL;
            if (i > 0 && i < N && j > 0 && j < N) {
                if (sim.chance(50)) {  // on a pixel centre: edges pass exactly through samples
                    x = (x & ~15) + 8;
                    y = (y & ~15) + 8;
                } else {
                    x += sim.rand_range(-60, 60);
                    y += sim.rand_range(-60, 60);
                }
            }
            grid[j][i] = {static_cast<int16_t>(x), static_cast<int16_t>(y), 1000, 0x808080, 0};
        }
    for (int j = 0; j < N; ++j)
        for (int i = 0; i < N; ++i) {
            const ScreenVtx &a = grid[j][i], &b = grid[j][i + 1], &c = grid[j + 1][i], &e = grid[j + 1][i + 1];
            if (sim.chance(50)) {
                add(a, b, e);
                add(a, e, c);
            } else {
                add(a, b, c);
                add(b, e, c);
            }
        }
    run();
    int twice = 0, holes = 0;
    for (const auto& [pix, n] : h.coverage)
        if (n > 1) ++twice;
    for (int py = Y0 / 16 + 1; py < (Y0 + N * CELL) / 16 - 1; ++py)
        for (int px = X0 / 16 + 1; px < (X0 + N * CELL) / 16 - 1; ++px)
            if (!h.coverage.count(py * 1024u + px)) ++holes;
    std::printf("  fill rule: %zu pixels covered, %d written twice, %d interior holes\n", h.coverage.size(), twice,
                holes);
    CHECK_EQ(twice, 0);
    CHECK_EQ(holes, 0);

    return sim.finish();
}
