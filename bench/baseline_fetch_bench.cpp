// Throughput of the BASELINE vertex_fetch (git tag `baseline`), measured with
// the same memory agent and workload as tb/vertex_fetch_tb.cpp so the two
// numbers are directly comparable. Built by `make baseline-bench`.
#include "Vvertex_fetch.h"
#include "axi_mem.h"
#include "sim.h"

using Dut = Vvertex_fetch;

static uint64_t run(tb::Sim<Dut>& sim, uint32_t count) {
    Dut& d = *sim.dut;
    d.vbuf_base_addr = 0x1000'0000;
    d.vertex_count = count;
    d.stream_ready = 1;
    d.start_pulse = 1;
    sim.tick();
    d.start_pulse = 0;
    const uint64_t t0 = sim.cycle;
    uint64_t got = 0;
    while (d.busy) {
        sim.settle();
        got += d.stream_valid && d.stream_ready;
        sim.tick();
        if (sim.cycle - t0 > 1'000'000) FATAL("baseline fetch did not finish");
    }
    CHECK_EQ(got, static_cast<uint64_t>(count));
    return sim.cycle - t0;
}

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "baseline_vertex_fetch");
    tb::AxiMem<Dut> mem(sim, 0x1000'0000, 64 * 1024);
    sim.add_agent(&mem);
    mem.reset_signals();
    sim.dut->start_pulse = 0;
    sim.reset();
    const uint64_t t0 = run(sim, 256);
    mem.latency = 32;
    const uint64_t t32 = run(sim, 256);
    std::printf("  baseline vertex_fetch, 256 vertices, ideal memory:     %llu cycles (%.2f cycles/vertex)\n",
                (unsigned long long)t0, t0 / 256.0);
    std::printf("  baseline vertex_fetch, 256 vertices, 32-cycle latency: %llu cycles (%.2f cycles/vertex)\n",
                (unsigned long long)t32, t32 / 256.0);
    return sim.finish();
}
