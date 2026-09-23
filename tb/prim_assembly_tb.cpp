// prim_assembly: random vertex streams grouped into triangles in order under
// random handshakes; back-to-back throughput; flush drops a partial triangle.
#include <deque>

#include "Vprim_assembly.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vprim_assembly;
using model::ScreenVtx;

namespace {

struct Harness : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    std::vector<ScreenVtx> to_send;
    std::deque<ScreenVtx> expected;  // vertices of complete triangles, in order
    size_t sent = 0;
    uint64_t tris = 0;
    int valid_pct = 100, ready_pct = 100;
    bool last_hs = false;
    explicit Harness(tb::Sim<Dut>& s) : sim(s), d(*s.dut) {}

    void observe() override {
        last_hs = d.in_valid && d.in_ready;
        if (!d.rst_n) return;
        if (last_hs) ++sent;
        if (d.out_valid && d.out_ready) {
            for (int k = 0; k < 3; ++k) {
                if (expected.empty()) FATAL("unexpected triangle");
                const ScreenVtx got{static_cast<int16_t>(d.out_sx[k]), static_cast<int16_t>(d.out_sy[k]),
                                    d.out_z[k], d.out_color[k], d.out_flags[k]};
                CHECK_MSG(got == expected.front(), "triangle %llu vertex %d mismatch", (unsigned long long)tris, k);
                expected.pop_front();
            }
            ++tris;
        }
    }
    void drive() override {
        if (!(d.in_valid && !last_hs)) {
            d.in_valid = sent < to_send.size() && sim.chance(valid_pct);
            if (sent < to_send.size()) {
                const ScreenVtx& v = to_send[sent];
                d.in_sx = static_cast<uint16_t>(v.sx);
                d.in_sy = static_cast<uint16_t>(v.sy);
                d.in_z = v.z;
                d.in_color = v.color;
                d.in_flags = v.flags;
            }
        }
        d.out_ready = sim.chance(ready_pct);
    }
};

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "prim_assembly");
    Dut& d = *sim.dut;
    Harness h(sim);
    sim.add_agent(&h);
    d.in_valid = 0;
    d.out_ready = 1;
    d.flush = 0;
    sim.reset();

    auto rnd = [&]() {
        return ScreenVtx{static_cast<int16_t>(sim.rand_u32()), static_cast<int16_t>(sim.rand_u32()),
                         static_cast<uint16_t>(sim.rand_u32()), sim.rand_u32() & 0xFFFFFF,
                         static_cast<uint8_t>(sim.rand_u32())};
    };
    // Queue n vertices; the complete triangles among them become expectations.
    auto queue = [&](size_t n) {
        const size_t base = h.to_send.size();
        for (size_t i = 0; i < n; ++i) h.to_send.push_back(rnd());
        for (size_t i = 0; i + 3 <= n; i += 3)
            for (int k = 0; k < 3; ++k) h.expected.push_back(h.to_send[base + i + k]);
    };
    auto drain = [&]() {
        const uint64_t t0 = sim.cycle;
        while (h.sent < h.to_send.size() || !d.idle) {
            sim.tick();
            if (sim.cycle - t0 > 100000) FATAL("stalled");
        }
        return sim.cycle - t0;
    };
    auto flush = [&]() {
        d.flush = 1;
        sim.tick();
        d.flush = 0;
    };

    // Back-to-back: 3 cycles per triangle, no bubbles.
    queue(300);
    const uint64_t t = drain();
    CHECK_EQ(h.tris, 100u);
    CHECK_MSG(t <= 300 + 3, "assembly inserted bubbles (%llu cycles for 300 vertices)", (unsigned long long)t);

    // Random handshakes, and draws whose length is not a multiple of 3: the
    // leftover vertices must be discarded by the flush at the next draw.
    for (int draw = 0; draw < 50; ++draw) {
        h.valid_pct = sim.rand_range(20, 100);
        h.ready_pct = sim.rand_range(20, 100);
        queue(sim.rand_range(0, 40));
        drain();
        CHECK(h.expected.empty());
        flush();
    }
    CHECK_EQ(d.idle, 1);
    return sim.finish();
}
