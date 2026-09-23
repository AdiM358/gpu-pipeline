// geom_engine: bit-exact against model::transform under random stimulus and
// back-pressure; throughput of one vertex per 4 cycles; idle signalling.
#include <deque>

#include "Vgeom_engine.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vgeom_engine;

namespace {

struct Stream : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    const int32_t* mvp;
    std::deque<model::ClipVtx> expected;
    std::vector<model::Vertex> to_send;
    size_t sent = 0;
    uint64_t received = 0;
    int valid_pct = 100, ready_pct = 100;
    Stream(tb::Sim<Dut>& s, const int32_t* m) : sim(s), d(*s.dut), mvp(m) {}

    void present() {
        d.in_valid = sent < to_send.size() && sim.chance(valid_pct);
        if (sent < to_send.size()) {
            const auto& v = to_send[sent];
            d.in_x = v.x;
            d.in_y = v.y;
            d.in_z = v.z;
            d.in_color = v.color & 0xFFFFFF;  // 24-bit port: Verilator does not mask inputs
        }
    }
    void observe() override {
        last_hs_ = d.in_valid && d.in_ready;
        if (!d.rst_n) return;
        if (d.in_valid && d.in_ready) {
            const auto& v = to_send[sent++];
            expected.push_back(model::transform(mvp, v.x, v.y, v.z, v.color & 0xFFFFFF));
        }
        if (d.out_valid && d.out_ready) {
            if (expected.empty()) FATAL("unexpected output vertex");
            const model::ClipVtx got{d.out_x, d.out_y, d.out_z, d.out_w, d.out_color};
            const model::ClipVtx& e = expected.front();
            CHECK_MSG(got == e, "vertex %llu: got (%08x %08x %08x %08x) expected (%08x %08x %08x %08x)",
                      (unsigned long long)received, got.x, got.y, got.z, got.w, e.x, e.y, e.z, e.w);
            CHECK_EQ(got.color, e.color);
            expected.pop_front();
            ++received;
        }
    }
    void drive() override {
        // Hold VALID and data stable until the handshake (stream protocol).
        const bool holding = d.in_valid && !last_hs_;
        if (!holding) present();
        d.out_ready = sim.chance(ready_pct);
    }
    bool last_hs_ = false;
};

int32_t rand_q16(tb::Sim<Dut>& sim, int range_bits) {
    // Random value with magnitude < 2^range_bits (Q16.16).
    const int64_t r = static_cast<int64_t>(sim.rand_u32() & ((1ull << range_bits) - 1));
    return static_cast<int32_t>(sim.chance(50) ? r : -r);
}

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "geom_engine");
    Dut& d = *sim.dut;
    int32_t mvp[16] = {};
    Stream st(sim, mvp);
    sim.add_agent(&st);
    auto load_mvp = [&]() {
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 4; ++c) d.mvp[r][c] = mvp[4 * r + c];
    };

    d.in_valid = 0;
    d.out_ready = 1;
    load_mvp();
    sim.reset();
    CHECK_EQ(d.idle, 1);

    auto run = [&](size_t n) {
        const uint64_t t0 = sim.cycle;
        while (st.received < n) {
            sim.tick();
            if (sim.cycle - t0 > 100 * n + 100) FATAL("stream stalled");
        }
        return sim.cycle - t0;
    };

    // ---- identity, then a scale+translate: exact expected values
    for (int i = 0; i < 4; ++i) mvp[5 * i] = 0x10000;
    load_mvp();
    st.to_send = {{0x10000, 0x20000, 0x30000, 0xFF0000}};
    run(1);
    mvp[0] = mvp[5] = mvp[10] = 0x20000;
    mvp[3] = 0x50000;  // translate x by 5
    load_mvp();
    st.to_send.push_back({-0x10000, 0x8000, 0, 0x00FF00});
    st.received = 0;
    const model::ClipVtx e = model::transform(mvp, -0x10000, 0x8000, 0, 0);
    CHECK_EQ(e.x, 0x30000);  // (-1 * 2) + 5 = 3
    CHECK_EQ(e.y, 0x10000);
    CHECK_EQ(e.w, 0x10000);
    run(1);

    // ---- throughput: back-to-back vertices, no back-pressure
    st.to_send.clear();
    st.sent = 0;
    st.received = 0;
    for (int i = 0; i < 400; ++i)
        st.to_send.push_back({rand_q16(sim, 20), rand_q16(sim, 20), rand_q16(sim, 20), sim.rand_u32()});
    const uint64_t t = run(400);
    std::printf("  400 vertices back-to-back: %llu cycles (%.2f cycles/vertex)\n",
                (unsigned long long)t, t / 400.0);
    CHECK_MSG(t <= 4 * 400 + 10, "throughput below 1 vertex / 4 cycles (%llu cycles)",
              (unsigned long long)t);

    // ---- random matrices (including overflow-sized entries), random handshakes
    for (int round = 0; round < 40; ++round) {
        const int mbits = sim.chance(20) ? 31 : sim.rand_range(12, 20);
        for (int i = 0; i < 16; ++i) mvp[i] = rand_q16(sim, mbits);
        load_mvp();
        st.valid_pct = sim.rand_range(20, 100);
        st.ready_pct = sim.rand_range(20, 100);
        st.to_send.clear();
        st.sent = 0;
        st.received = 0;
        const int vbits = sim.chance(20) ? 31 : sim.rand_range(14, 22);
        for (int i = 0; i < 100; ++i)
            st.to_send.push_back({rand_q16(sim, vbits), rand_q16(sim, vbits), rand_q16(sim, vbits),
                                  sim.rand_u32()});
        run(100);
        CHECK(st.expected.empty());
    }

    // ---- idle only once everything has drained
    st.valid_pct = st.ready_pct = 100;
    st.to_send.clear();
    st.sent = 0;
    st.received = 0;
    for (int i = 0; i < 3; ++i) st.to_send.push_back({i, i, i, 0});
    int busy_cycles = 0;
    bool was_busy = false;
    do {
        sim.tick();
        was_busy |= !d.idle;
        CHECK_MSG(!d.idle || st.expected.empty(), "idle asserted with a vertex in flight");
        if (++busy_cycles > 100) FATAL("never idle");
    } while (!(d.idle && st.received == 3));
    CHECK(was_busy);
    CHECK(st.expected.empty());

    return sim.finish();
}
