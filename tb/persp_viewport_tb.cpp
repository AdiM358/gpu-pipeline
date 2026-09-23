// persp_viewport: bit-exact against model::persp_viewport for visible,
// near/behind-eye, outside-frustum, guard-band and full-range vertices, under
// random back-pressure; one vertex per cycle throughput.
#include <deque>

#include "Vpersp_viewport.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vpersp_viewport;

namespace {

const model::Config kCfg;  // 320x240, matches the RTL defaults

struct Stream : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    std::deque<model::ScreenVtx> expected;
    std::vector<model::ClipVtx> to_send;
    size_t sent = 0;
    uint64_t received = 0;
    int valid_pct = 100, ready_pct = 100;
    bool last_hs = false;
    uint32_t flags_seen = 0;  // OR of all output flag bits (coverage of the checks)
    explicit Stream(tb::Sim<Dut>& s) : sim(s), d(*s.dut) {}

    void observe() override {
        last_hs = d.in_valid && d.in_ready;
        if (!d.rst_n) return;
        if (last_hs) expected.push_back(model::persp_viewport(to_send[sent++], kCfg));
        if (d.out_valid && d.out_ready) {
            if (expected.empty()) FATAL("unexpected output");
            const model::ScreenVtx got{static_cast<int16_t>(d.out_sx), static_cast<int16_t>(d.out_sy),
                                       d.out_z, d.out_color, d.out_flags};
            const model::ScreenVtx& e = expected.front();
            const model::ClipVtx& c = to_send[received];
            CHECK_MSG(got == e,
                      "clip (%d %d %d %d): got sx=%d sy=%d z=%u f=%02x, expected sx=%d sy=%d z=%u f=%02x",
                      c.x, c.y, c.z, c.w, got.sx, got.sy, got.z, got.flags, e.sx, e.sy, e.z, e.flags);
            flags_seen |= got.flags;
            expected.pop_front();
            ++received;
        }
    }
    void drive() override {
        if (!(d.in_valid && !last_hs)) {
            d.in_valid = sent < to_send.size() && sim.chance(valid_pct);
            if (sent < to_send.size()) {
                const auto& v = to_send[sent];
                d.in_x = v.x;
                d.in_y = v.y;
                d.in_z = v.z;
                d.in_w = v.w;
                d.in_color = v.color;
            }
        }
        d.out_ready = sim.chance(ready_pct);
    }
};

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "persp_viewport");
    Dut& d = *sim.dut;
    Stream st(sim);
    sim.add_agent(&st);
    d.in_valid = 0;
    d.out_ready = 1;
    sim.reset();
    CHECK_EQ(d.idle, 1);

    auto run = [&]() {
        const uint64_t t0 = sim.cycle;
        while (st.received < st.to_send.size()) {
            sim.tick();
            if (sim.cycle - t0 > 50 * st.to_send.size() + 200) FATAL("stream stalled");
        }
        return sim.cycle - t0;
    };
    auto q = [](double v) { return static_cast<int32_t>(v * 65536.0); };

    // ---- analytic spot check: (1, 1, 0.5, w=2) -> x/w = y/w = 0.5, z/w = 0.25
    const model::ScreenVtx a = model::persp_viewport({q(1), q(1), q(0.5), q(2), 0x123456}, kCfg);
    CHECK_EQ(a.sx, 16 * 240);  // 320/2 * 1.5 px
    CHECK_EQ(a.sy, 16 * 60);   // 240/2 * 0.5 px
    CHECK_EQ(a.z, 40959);      // floor(1.25/2 * 65535)
    CHECK_EQ(a.flags, 0);
    st.to_send.push_back({q(1), q(1), q(0.5), q(2), 0x123456});

    // ---- directed boundary cases
    const int32_t wn = model::kWNear;
    for (int32_t w : {wn - 1, wn, wn + 1, 0, -1, -0x10000, INT32_MIN, INT32_MAX, 0x2000})
        st.to_send.push_back({q(0.1), q(-0.1), q(0.2), w, 0xABCDEF});
    // Exactly on the frustum planes (inside) and one ulp outside.
    for (int32_t dlt : {0, 1})
        st.to_send.push_back({q(1) + dlt, -q(1) - dlt, q(1) + dlt, q(1), 0});
    // Guard band edges: sx = 8W(1 + x/w) crosses +/-16384 near x/w = 5.4 and -7.4
    for (double r : {5.39, 5.40, 5.41, -7.39, -7.40, -7.41})
        st.to_send.push_back({q(r), q(r * 0.75), 0, q(1), 0});
    run();

    // ---- throughput: one vertex per cycle once the pipeline is full
    st.to_send.clear();
    st.sent = st.received = 0;
    for (int i = 0; i < 500; ++i) st.to_send.push_back({q(0.1), q(0.2), q(0.3), q(1.0 + i), 0});
    const uint64_t t = run();
    std::printf("  500 vertices back-to-back: %llu cycles (latency + 1/cycle)\n", (unsigned long long)t);
    CHECK_MSG(t <= 500 + 40, "persp_viewport not at 1 vertex/cycle (%llu cycles)", (unsigned long long)t);

    // ---- random classes under random handshakes
    for (int round = 0; round < 30; ++round) {
        st.to_send.clear();
        st.sent = st.received = 0;
        st.valid_pct = sim.rand_range(30, 100);
        st.ready_pct = sim.rand_range(30, 100);
        for (int i = 0; i < 200; ++i) {
            model::ClipVtx c{};
            c.color = sim.rand_u32() & 0xFFFFFF;
            switch (sim.rand_range(0, 3)) {
                case 0: {  // visible-ish, perspective w in [0.1, 100)
                    const double w = 0.1 + 100.0 * (sim.rand_u32() / 4294967296.0);
                    auto r = [&]() { return (sim.rand_u32() / 2147483648.0 - 1.0) * 1.2 * w; };
                    c = {q(r()), q(r()), q(r()), q(w), c.color};
                    break;
                }
                case 1:  // near / behind the eye
                    c = {static_cast<int32_t>(sim.rand_u32() >> 8), static_cast<int32_t>(sim.rand_u32() >> 8),
                         static_cast<int32_t>(sim.rand_u32() >> 8), sim.rand_range(-0x20000, 0x1100), c.color};
                    break;
                case 2:  // small w: huge screen coordinates, guard band
                    c = {sim.rand_range(-0x40000, 0x40000), sim.rand_range(-0x40000, 0x40000),
                         sim.rand_range(-0x40000, 0x40000), sim.rand_range(0x1001, 0x8000), c.color};
                    break;
                default:  // anything
                    c = {static_cast<int32_t>(sim.rand_u32()), static_cast<int32_t>(sim.rand_u32()),
                         static_cast<int32_t>(sim.rand_u32()), static_cast<int32_t>(sim.rand_u32()), c.color};
            }
            st.to_send.push_back(c);
        }
        run();
    }
    // Every flag bit must have been produced (and checked) at least once.
    CHECK_EQ(st.flags_seen, 0xFFu);

    // ---- idle returns once drained
    for (int i = 0; i < 5 && !d.idle; ++i) sim.tick();
    CHECK_EQ(d.idle, 1);
    return sim.finish();
}
