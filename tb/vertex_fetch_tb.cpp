// vertex_fetch: ordering/completeness scoreboard under random memory timing
// and back-pressure, 4 KB burst splitting, error handling, prefetch overlap.
#include <deque>

#include "Vvertex_fetch.h"
#include "axi_mem.h"
#include "sim.h"

using Dut = Vvertex_fetch;

namespace {

constexpr uint32_t kBase = 0x1000'0000;
constexpr uint32_t kMemBytes = 64 * 1024;

struct Vtx {
    uint32_t x, y, z, c;
    bool operator==(const Vtx& o) const { return x == o.x && y == o.y && z == o.z && c == o.c; }
};

// Consumes the vertex stream with random back-pressure and compares every
// vertex against the expected sequence.
struct Sink : tb::Agent {
    tb::Sim<Dut>& sim;
    Dut& d;
    std::deque<Vtx> expected;
    uint64_t received = 0, unexpected = 0;
    int ready_pct = 100;
    explicit Sink(tb::Sim<Dut>& s) : sim(s), d(*s.dut) {}
    void observe() override {
        if (!d.rst_n || !(d.out_valid && d.out_ready)) return;
        const Vtx got{d.out_x, d.out_y, d.out_z, d.out_color};
        if (expected.empty()) {
            ++unexpected;
            return;
        }
        CHECK_MSG(got == expected.front(), "vertex %llu mismatch: got (%08x %08x %08x %08x)",
                  (unsigned long long)received, got.x, got.y, got.z, got.c);
        expected.pop_front();
        ++received;
    }
    void drive() override { d.out_ready = sim.chance(ready_pct); }
};

// Counts error_set pulses.
struct ErrMon : tb::Agent {
    Dut& d;
    int pulses = 0;
    explicit ErrMon(Dut& dut) : d(dut) {}
    void observe() override {
        if (d.rst_n && d.error_set) ++pulses;
    }
};

Vtx vertex_at(tb::AxiMem<Dut>& mem, uint32_t addr) {
    return {mem.read32(addr), mem.read32(addr + 4), mem.read32(addr + 8), mem.read32(addr + 12)};
}

// Starts a job and runs until busy falls. Returns cycles taken.
uint64_t run_job(tb::Sim<Dut>& sim, uint32_t base, uint32_t count, uint64_t timeout = 200000) {
    Dut& d = *sim.dut;
    d.base_addr = base;
    d.vertex_count = count;
    d.start = 1;
    sim.tick();
    d.start = 0;
    const uint64_t t0 = sim.cycle;
    while (d.busy) {
        sim.tick();
        if (sim.cycle - t0 > timeout) FATAL("job at 0x%08x (%u vertices) did not finish", base, count);
    }
    return sim.cycle - t0;
}

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "vertex_fetch");
    Dut& d = *sim.dut;
    tb::AxiMem<Dut> mem(sim, kBase, kMemBytes);
    Sink sink(sim);
    ErrMon errmon(d);
    sim.add_agent(&mem);
    sim.add_agent(&sink);
    sim.add_agent(&errmon);

    for (uint32_t a = kBase; a < kBase + kMemBytes; a += 4) mem.write32(a, sim.rand_u32());
    mem.reset_signals();
    d.start = 0;
    d.out_ready = 1;
    sim.reset();
    CHECK_EQ(d.busy, 0);

    auto expect_range = [&](uint32_t base, uint32_t count) {
        for (uint32_t i = 0; i < count; ++i) sink.expected.push_back(vertex_at(mem, base + 16 * i));
    };

    // ---- zero-length job: never busy for more than the start cycle
    run_job(sim, kBase, 0);
    CHECK_EQ(mem.bursts, 0u);

    // ---- ideal memory: throughput is bus-limited at 4 cycles per vertex
    expect_range(kBase, 256);
    const uint64_t t_ideal = run_job(sim, kBase, 256);
    CHECK(sink.expected.empty());
    std::printf("  256 vertices, ideal memory:        %llu cycles (%.2f cycles/vertex)\n",
                (unsigned long long)t_ideal, t_ideal / 256.0);
    CHECK_MSG(t_ideal <= 4 * 256 + 16, "ideal-memory fetch took %llu cycles", (unsigned long long)t_ideal);

    // ---- 32-cycle read latency: prefetch keeps several bursts in flight
    mem.latency = 32;
    mem.peak_outstanding = 0;
    expect_range(kBase, 256);
    const uint64_t t_lat = run_job(sim, kBase, 256);
    CHECK(sink.expected.empty());
    std::printf("  256 vertices, 32-cycle latency:    %llu cycles (%.2f cycles/vertex), peak %zu bursts in flight\n",
                (unsigned long long)t_lat, t_lat / 256.0, mem.peak_outstanding);
    CHECK_MSG(mem.peak_outstanding >= 3, "prefetch not overlapping bursts (peak %zu)", mem.peak_outstanding);
    CHECK_MSG(t_lat <= 4 * 256 + 64, "latency not hidden: %llu cycles", (unsigned long long)t_lat);
    mem.latency = 0;

    // ---- bursts are split at 4 KB boundaries (AxiMem checks every burst)
    const uint32_t near_4k = kBase + 0x1000 - 16 * 2;  // 2 vertices before the boundary
    expect_range(near_4k, 11);
    const uint64_t bursts_before = mem.bursts;
    run_job(sim, near_4k, 11);
    CHECK(sink.expected.empty());
    CHECK_EQ(mem.bursts - bursts_before, 4u);  // 2 | 4 | 4 | 1

    // ---- random jobs under random timing and back-pressure
    for (int job = 0; job < 60; ++job) {
        mem.ar_ready_pct = sim.rand_range(20, 100);
        mem.r_valid_pct = sim.rand_range(20, 100);
        mem.latency = sim.rand_range(0, 20);
        sink.ready_pct = sim.rand_range(10, 100);
        const uint32_t count = sim.chance(10) ? sim.rand_range(0, 3) : sim.rand_range(1, 400);
        const uint32_t base = kBase + 16 * sim.rand_range(0, (kMemBytes - 16 * 400) / 16 - 1);
        expect_range(base, count);
        run_job(sim, base, count);
        CHECK_MSG(sink.expected.empty(), "job %d: %zu vertices missing", job, sink.expected.size());
        sink.expected.clear();
        CHECK(mem.idle());
    }
    CHECK_EQ(sink.unexpected, 0u);
    CHECK_EQ(errmon.pulses, 0);
    mem.ar_ready_pct = mem.r_valid_pct = sink.ready_pct = 100;
    mem.latency = 0;

    // ---- SLVERR mid-job: one error pulse, output is a prefix, bus drains
    {
        const uint32_t base = kBase + 0x2000, count = 100;
        mem.err_lo = base + 16 * 37 + 8;  // z word of vertex 37
        mem.err_hi = mem.err_lo + 4;
        sink.ready_pct = 50;
        const uint64_t before = sink.received;
        expect_range(base, count);
        run_job(sim, base, count);
        const uint64_t got = sink.received - before;
        CHECK_EQ(errmon.pulses, 1);
        CHECK_MSG(got <= 37, "%llu vertices delivered, vertex 37 or later leaked past the error",
                  (unsigned long long)got);
        CHECK(mem.idle());
        sink.expected.clear();
        mem.err_lo = mem.err_hi = 0;
        sink.ready_pct = 100;
    }

    // ---- RLAST on the wrong beat is a protocol error too
    {
        mem.bad_rlast_burst = static_cast<int>(mem.bursts) + 2;  // third burst of the job
        expect_range(kBase, 64);
        run_job(sim, kBase, 64);
        CHECK_EQ(errmon.pulses, 2);
        CHECK(mem.idle());
        sink.expected.clear();
        mem.bad_rlast_burst = -1;
    }

    // ---- a clean job after errors works normally
    expect_range(kBase + 0x3000, 50);
    run_job(sim, kBase + 0x3000, 50);
    CHECK(sink.expected.empty());
    CHECK_EQ(errmon.pulses, 2);
    CHECK_EQ(sink.unexpected, 0u);

    return sim.finish();
}
