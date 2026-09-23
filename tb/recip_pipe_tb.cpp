// recip_pipe: floor(2^44/d) for boundary and random divisors, one result per
// cycle, payload alignment, and stall behaviour.
#include <deque>

#include "Vrecip_pipe.h"
#include "gpu_model.h"
#include "sim.h"

using Dut = Vrecip_pipe;

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "recip_pipe");
    Dut& d = *sim.dut;
    d.en = 1;
    d.in_valid = 0;
    d.in_d = 0;
    d.in_pay = 0;
    sim.reset();

    std::vector<uint32_t> divisors = {0x1001, 0x1002, 0x1FFF, 0x2000, 0x10000, 0x10001,
                                      0x7FFFFFFF, 0x7FFFFFFE, 0x40000000, 0x3FFFFFFF, 12345678};
    for (int i = 0; i < 20000; ++i) {
        // Mix of magnitudes: uniform in the exponent, then random mantissa.
        const int bits = sim.rand_range(13, 31);
        uint32_t v = (sim.rand_u32() & ((1u << bits) - 1)) | (1u << (bits - 1));
        if (v <= 0x1000) v = 0x1001;
        divisors.push_back(v);
    }

    std::deque<std::pair<uint32_t, uint32_t>> expected;  // (quotient, payload)
    size_t next = 0;
    uint64_t results = 0, cycles_streaming = 0;
    while (next < divisors.size() || !expected.empty()) {
        // Random stalls: while en is low nothing may move.
        d.en = sim.chance(85);
        const bool issue = next < divisors.size() && sim.chance(90);
        d.in_valid = issue;
        if (issue) {
            d.in_d = divisors[next];
            d.in_pay = static_cast<uint32_t>(next);
        }
        sim.settle();
        if (d.out_valid && d.en) {
            if (expected.empty()) FATAL("unexpected result");
            CHECK_MSG(d.out_q == expected.front().first, "d=0x%x: got 0x%x expected 0x%x",
                      divisors[expected.front().second], d.out_q, expected.front().first);
            CHECK_EQ(d.out_pay, expected.front().second);
            expected.pop_front();
            ++results;
        }
        if (issue && d.en) {
            expected.push_back({model::recip_w(static_cast<int32_t>(divisors[next])),
                                static_cast<uint32_t>(next)});
            ++next;
        }
        sim.tick();
        ++cycles_streaming;
        if (cycles_streaming > 10 * divisors.size() + 1000) FATAL("pipeline stuck");
    }
    CHECK_EQ(results, divisors.size());

    // Full throughput: 1000 back-to-back operands emerge on 1000 consecutive
    // cycles after a 32-cycle latency.
    d.en = 1;
    uint64_t first_out = 0, last_out = 0, n_out = 0;
    for (int t = 0; t < 1100; ++t) {
        d.in_valid = t < 1000;
        d.in_d = 0x10000 + t;
        d.in_pay = t;
        sim.settle();
        if (d.out_valid) {
            if (n_out == 0) first_out = t;
            last_out = t;
            CHECK_EQ(d.out_q, model::recip_w(0x10000 + static_cast<int32_t>(d.out_pay)));
            ++n_out;
        }
        sim.tick();
    }
    CHECK_EQ(n_out, 1000u);
    CHECK_EQ(first_out, 32u);
    CHECK_EQ(last_out - first_out, 999u);

    return sim.finish();
}
