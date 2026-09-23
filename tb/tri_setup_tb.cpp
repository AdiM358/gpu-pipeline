// tri_setup: fate (raster / culled / clipped) and every output field
// bit-exact against model::setup, for random and directed triangles, all
// culling configurations, and random back-pressure.
#include <algorithm>
#include <deque>

#include "Vtri_setup.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vtri_setup;
using model::ScreenVtx;

namespace {

const model::Config kCfg;

uint64_t mask_bits(int64_t v, int bits) { return static_cast<uint64_t>(v) & ((1ULL << bits) - 1); }

struct Tri {
    ScreenVtx v[3];
    bool cull_back, front_cw;
};

struct Expect {
    model::TriFate fate;
    model::TriSetup t;
    uint64_t accepted_at;
};

struct Harness : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    std::vector<Tri> to_send;
    std::deque<Expect> expected;
    size_t sent = 0, done = 0;
    int valid_pct = 100, ready_pct = 100;
    bool last_hs = false;
    uint64_t n_raster = 0, n_culled = 0, n_clipped = 0;
    uint64_t lat_sum = 0, lat_n = 0, lat_min = ~0ull, lat_max = 0;  // accept -> out_valid, rasterized
    bool prev_out_valid = false;
    explicit Harness(tb::Sim<Dut>& s) : sim(s), d(*s.dut) {}

    void observe() override {
        last_hs = d.in_valid && d.in_ready;
        if (!d.rst_n) return;
        if (last_hs) {
            const Tri& t = to_send[sent++];
            Expect e{};
            e.fate = model::setup(t.v, t.cull_back, t.front_cw, kCfg, &e.t);
            e.accepted_at = sim.cycle;
            expected.push_back(e);
        }
        if (d.out_valid && !prev_out_valid && !expected.empty()) {
            const uint64_t lat = sim.cycle - expected.front().accepted_at;
            lat_sum += lat, ++lat_n;
            lat_min = std::min(lat_min, lat), lat_max = std::max(lat_max, lat);
        }
        prev_out_valid = d.out_valid;
        // Outcomes are not in program order: while an older triangle waits in
        // the output register, newer ones can already be culled or clipped.
        // The output register always holds the oldest unresolved triangle.
        if (d.out_valid && d.out_ready) {
            if (expected.empty()) FATAL("output with no triangle in flight");
            const Expect e = expected.front();
            expected.pop_front();
            ++n_raster;
            CHECK_MSG(e.fate == model::TriFate::Raster, "triangle %zu: RTL rasterized, model fate %d", done,
                      int(e.fate));
            if (e.fate == model::TriFate::Raster) compare(e.t, done);
            ++done;
        }
        CHECK_MSG(!(d.culled && d.clipped), "culled and clipped in the same cycle");
        if (d.culled || d.clipped) {
            // Skip a raster triangle still parked in the output register.
            size_t k = (d.out_valid && !d.out_ready && !expected.empty() &&
                        expected.front().fate == model::TriFate::Raster) ? 1 : 0;
            if (k >= expected.size()) FATAL("cull/clip pulse with no triangle in flight");
            const Expect e = expected[k];
            expected.erase(expected.begin() + k);
            const model::TriFate got = d.culled ? model::TriFate::Culled : model::TriFate::Clipped;
            (d.culled ? n_culled : n_clipped)++;
            CHECK_MSG(e.fate == got, "triangle %zu: RTL fate %d, model fate %d", done, int(got), int(e.fate));
            ++done;
        }
    }

    void compare(const model::TriSetup& m, size_t idx) {
        bool ok = d.out_px0 == m.px0 && d.out_px1 == m.px1 && d.out_py0 == m.py0 && d.out_py1 == m.py1;
        for (int e = 0; e < 3; ++e) {
            ok &= d.out_e0[e] == mask_bits(m.e0[e], 34);
            ok &= d.out_ex[e] == mask_bits(m.ex[e], 21);
            ok &= d.out_ey[e] == mask_bits(m.ey[e], 21);
        }
        for (int i = 0; i < 4; ++i) {
            ok &= d.out_a0[i] == mask_bits(m.a0[i], 38);
            ok &= d.out_ax[i] == mask_bits(m.ax[i], 38);
            ok &= d.out_ay[i] == mask_bits(m.ay[i], 38);
            ok &= d.out_amin[i] == m.amin[i] && d.out_amax[i] == m.amax[i];
        }
        CHECK_MSG(ok, "triangle %zu: setup fields differ (bbox RTL %u..%u,%u..%u model %d..%d,%d..%d; "
                      "e0[0] RTL %llx model %llx; a0[0] RTL %llx model %llx; ax[0] RTL %llx model %llx)",
                  idx, d.out_px0, d.out_px1, d.out_py0, d.out_py1, m.px0, m.px1, m.py0, m.py1,
                  (unsigned long long)d.out_e0[0], (unsigned long long)mask_bits(m.e0[0], 34),
                  (unsigned long long)d.out_a0[0], (unsigned long long)mask_bits(m.a0[0], 38),
                  (unsigned long long)d.out_ax[0], (unsigned long long)mask_bits(m.ax[0], 38));
    }

    void drive() override {
        if (!(d.in_valid && !last_hs)) {
            d.in_valid = sent < to_send.size() && sim.chance(valid_pct);
            if (sent < to_send.size()) {
                const Tri& t = to_send[sent];
                for (int k = 0; k < 3; ++k) {
                    d.in_sx[k] = static_cast<uint16_t>(t.v[k].sx);
                    d.in_sy[k] = static_cast<uint16_t>(t.v[k].sy);
                    d.in_z[k] = t.v[k].z;
                    d.in_color[k] = t.v[k].color & 0xFFFFFF;
                    d.in_flags[k] = t.v[k].flags;
                }
                d.cull_back = t.cull_back;
                d.front_cw = t.front_cw;
            }
        }
        d.out_ready = sim.chance(ready_pct);
    }
};

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "tri_setup");
    Dut& d = *sim.dut;
    Harness h(sim);
    sim.add_agent(&h);
    d.in_valid = 0;
    d.out_ready = 1;
    d.cull_back = 0;
    d.front_cw = 0;
    sim.reset();
    CHECK_EQ(d.idle, 1);

    auto rnd_vtx = [&](int lo, int hi) {
        return ScreenVtx{static_cast<int16_t>(sim.rand_range(lo, hi)), static_cast<int16_t>(sim.rand_range(lo, hi)),
                         static_cast<uint16_t>(sim.rand_u32()), sim.rand_u32() & 0xFFFFFF, 0};
    };
    auto add = [&](ScreenVtx a, ScreenVtx b, ScreenVtx c) {
        h.to_send.push_back({{a, b, c}, sim.chance(50), sim.chance(50)});
    };
    auto run = [&]() {
        const uint64_t t0 = sim.cycle;
        while (h.done < h.to_send.size()) {
            sim.tick();
            if (sim.cycle - t0 > 400 * h.to_send.size() + 1000) FATAL("setup stalled");
        }
        return sim.cycle - t0;
    };

    // ---- directed: a plain CCW (in NDC) triangle and its CW mirror
    const ScreenVtx p0{16 * 10, 16 * 50, 100, 0xFF0000, 0}, p1{16 * 60, 16 * 50, 200, 0x00FF00, 0},
                    p2{16 * 10, 16 * 10, 300, 0x0000FF, 0};
    for (bool cb : {false, true})
        for (bool fcw : {false, true}) {
            h.to_send.push_back({{p0, p1, p2}, cb, fcw});
            h.to_send.push_back({{p0, p2, p1}, cb, fcw});
        }
    // Degenerate: collinear and coincident vertices are always culled.
    h.to_send.push_back({{p0, p1, {16 * 110, 16 * 50, 0, 0, 0}}, false, false});
    h.to_send.push_back({{p0, p0, p0}, false, false});
    // Tiny triangle between pixel centres: empty bounding box -> clipped.
    h.to_send.push_back({{{16 * 5 + 9, 16 * 5 + 9, 0, 0, 0}, {16 * 5 + 14, 16 * 5 + 9, 0, 0, 0},
                          {16 * 5 + 9, 16 * 5 + 14, 0, 0, 0}}, false, false});
    // Entirely off-screen but inside the guard band: empty after clamping.
    h.to_send.push_back({{{-16 * 200, 16 * 10, 0, 0, 0}, {-16 * 100, 16 * 10, 0, 0, 0},
                          {-16 * 150, 16 * 90, 0, 0, 0}}, false, false});
    // Largest triangles the guard band allows (area close to 2^31).
    h.to_send.push_back({{{-16384, -16384, 0, 0xFFFFFF, 0}, {16383, -16384, 65535, 0, 0},
                          {-16384, 16383, 32768, 0x808080, 0}}, false, false});
    h.to_send.push_back({{{-16384, -16384, 0, 0, 0}, {-16384, 16383, 65535, 0, 0},
                          {16383, 16383, 1, 0, 0}}, false, false});
    // Flags: shared outcode, one NEAR vertex, one guard-band vertex.
    for (uint8_t f : {uint8_t(model::OC_XPOS), uint8_t(model::FLAG_NEAR), uint8_t(model::FLAG_GB)}) {
        ScreenVtx a = p0, b = p1, c = p2;
        a.flags = f;
        if (f == model::OC_XPOS) b.flags = c.flags = f | model::OC_YNEG;  // all share XPOS
        h.to_send.push_back({{a, b, c}, false, false});
    }
    // Outcodes on different planes only: not rejected.
    {
        ScreenVtx a = p0, b = p1, c = p2;
        a.flags = model::OC_XNEG;
        b.flags = model::OC_XPOS;
        c.flags = model::OC_YPOS;
        h.to_send.push_back({{a, b, c}, false, false});
    }
    run();

    // ---- random screen-space triangles of assorted sizes
    for (int i = 0; i < 1500; ++i) {
        switch (sim.rand_range(0, 4)) {
            case 0: add(rnd_vtx(-200, 5300), rnd_vtx(-200, 4000), rnd_vtx(-200, 5300)); break;  // on screen
            case 1: {  // small
                const ScreenVtx a = rnd_vtx(0, 5000);
                auto near = [&]() {
                    ScreenVtx v = rnd_vtx(0, 1);
                    v.sx = static_cast<int16_t>(a.sx + sim.rand_range(-40, 40));
                    v.sy = static_cast<int16_t>(a.sy + sim.rand_range(-40, 40));
                    return v;
                };
                add(a, near(), near());
                break;
            }
            case 2: add(rnd_vtx(-16384, 16383), rnd_vtx(-16384, 16383), rnd_vtx(-16384, 16383)); break;  // huge
            case 3: {  // sliver: nearly collinear
                const ScreenVtx a = rnd_vtx(0, 5000), b = rnd_vtx(0, 3800);
                ScreenVtx c = a;
                c.sx = static_cast<int16_t>((a.sx + b.sx) / 2 + sim.rand_range(-2, 2));
                c.sy = static_cast<int16_t>((a.sy + b.sy) / 2 + sim.rand_range(-2, 2));
                c.z = static_cast<uint16_t>(sim.rand_u32());
                add(a, b, c);
                break;
            }
            default: {  // axis-aligned edges exercise the top-left rule
                const int x0 = 16 * sim.rand_range(0, 300) + 8 * sim.rand_range(0, 1);
                const int y0 = 16 * sim.rand_range(0, 220) + 8 * sim.rand_range(0, 1);
                const int w = 16 * sim.rand_range(1, 20), hgt = 16 * sim.rand_range(1, 20);
                ScreenVtx a = rnd_vtx(0, 1), b = rnd_vtx(0, 1), c = rnd_vtx(0, 1);
                a.sx = x0; a.sy = y0;
                b.sx = x0 + w; b.sy = y0;
                c.sx = x0; c.sy = y0 + hgt;
                if (sim.chance(50)) c.sx = x0 + w;
                add(a, b, c);
            }
        }
    }
    h.valid_pct = 70;
    h.ready_pct = 60;
    run();

    // ---- realistic vertices from the model's own perspective stage (flags included)
    h.valid_pct = h.ready_pct = 100;
    auto clip = [&](double spread) {
        const double w = 0.05 + 3.0 * (sim.rand_u32() / 4294967296.0);
        auto r = [&]() { return (sim.rand_u32() / 2147483648.0 - 1.0) * spread * w; };
        model::ClipVtx c{static_cast<int32_t>(r() * 65536), static_cast<int32_t>(r() * 65536),
                         static_cast<int32_t>(r() * 65536), static_cast<int32_t>((sim.chance(5) ? -w : w) * 65536),
                         sim.rand_u32() & 0xFFFFFF};
        return model::persp_viewport(c, kCfg);
    };
    for (int i = 0; i < 1500; ++i) {
        const double spread = sim.chance(50) ? 1.0 : 4.0;
        add(clip(spread), clip(spread), clip(spread));
    }
    const size_t before = h.done;
    const uint64_t t = run();
    std::printf("  outcomes: %llu rasterized, %llu culled, %llu clipped; last batch %.1f cycles/triangle\n",
                (unsigned long long)h.n_raster, (unsigned long long)h.n_culled,
                (unsigned long long)h.n_clipped, double(t) / (h.done - before));
    CHECK(h.n_raster > 500 && h.n_culled > 200 && h.n_clipped > 200);
    std::printf("  setup latency, accept to output valid (rasterized triangles): min %llu, mean %.1f, max %llu cycles\n",
                (unsigned long long)h.lat_min, double(h.lat_sum) / h.lat_n, (unsigned long long)h.lat_max);

    for (int i = 0; i < 5; ++i) sim.tick();
    CHECK_EQ(d.idle, 1);
    return sim.finish();
}
