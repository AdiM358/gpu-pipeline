// axil_regs: AXI4-Lite protocol + register semantics against a reference model.
#include <map>

#include "Vaxil_regs.h"
#include "axil_master.h"
#include "gpu_regs.h"
#include "sim.h"

using Dut = Vaxil_regs;

namespace {

constexpr uint32_t OKAY = 0, SLVERR = 2;

// Counts command pulses and checks they are exactly one cycle wide.
struct PulseMonitor : tb::Agent {
    Dut* d;
    int draws = 0, clears = 0;
    bool prev_draw = false, prev_clear = false;
    explicit PulseMonitor(Dut* dut) : d(dut) {}
    void observe() override {
        if (!d->rst_n) return;  // flops hold random values until reset is applied
        if (d->cmd_draw) ++draws;
        if (d->cmd_clear) ++clears;
        CHECK_MSG(!(prev_draw && d->cmd_draw), "cmd_draw wider than one cycle");
        CHECK_MSG(!(prev_clear && d->cmd_clear), "cmd_clear wider than one cycle");
        prev_draw = d->cmd_draw;
        prev_clear = d->cmd_clear;
    }
};

// Raises done_set for exactly one rising edge, `countdown` edges after arming.
struct DonePulser : tb::Agent {
    Dut* d;
    int countdown = 0;
    explicit DonePulser(Dut* dut) : d(dut) {}
    void drive() override {
        d->done_set = 0;
        if (countdown > 0 && --countdown == 0) d->done_set = 1;
    }
};

// Reference model of the read/write registers (value masks per offset).
struct RefRegs {
    std::map<uint32_t, uint32_t> val;
    std::map<uint32_t, uint32_t> mask;
    RefRegs() {
        mask[GPU_REG_IRQ_EN] = 0x3;
        mask[GPU_REG_VBUF_BASE] = 0xFFFFFFF0;
        mask[GPU_REG_VERTEX_COUNT] = 0xFFFFFFFF;
        mask[GPU_REG_RASTER_CFG] = 0x3;
        mask[GPU_REG_CLEAR_COLOR] = 0xFFFF;
        for (uint32_t i = 0; i < 16; ++i) mask[GPU_REG_MVP0 + 4 * i] = 0xFFFFFFFF;
        for (auto& [k, m] : mask) val[k] = 0;
    }
    bool is_rw(uint32_t off) const { return mask.count(off) != 0; }
    void write(uint32_t off, uint32_t data, uint32_t strb) {
        uint32_t bytes = 0;
        for (int b = 0; b < 4; ++b)
            if (strb & (1u << b)) bytes |= 0xFFu << (8 * b);
        val[off] = ((val[off] & ~bytes) | (data & bytes)) & mask[off];
    }
};

bool is_mapped(uint32_t off) {
    return off <= GPU_REG_FB_INFO ||
           (off >= GPU_REG_PERF_CYCLES && off < GPU_REG_PERF_CYCLES + 4 * GPU_NUM_PERF) ||
           (off >= GPU_REG_MVP0 && off < GPU_REG_MVP0 + 64);
}

}  // namespace

int main(int argc, char** argv) {
    tb::Sim<Dut> sim(argc, argv, "axil_regs");
    Dut& d = *sim.dut;
    tb::AxilMaster<Dut> bus(sim);
    PulseMonitor pulses(&d);
    sim.add_agent(&pulses);
    DonePulser pulser(&d);
    sim.add_agent(&pulser);

    bus.idle();
    d.busy = 0;
    d.done_set = 0;
    d.err_set = 0;
    for (int i = 0; i < 8; ++i) d.perf[i] = 0;
    sim.reset();

    // ---- reset values and read-only identification registers
    CHECK_EQ(bus.read32(GPU_REG_ID), GPU_ID_VALUE);
    CHECK_EQ(bus.last_resp, OKAY);
    CHECK_EQ(bus.read32(GPU_REG_FB_INFO), (240u << 16) | 320u);
    CHECK_EQ(bus.read32(GPU_REG_STATUS), 0u);
    CHECK_EQ(bus.read32(GPU_REG_VBUF_BASE), 0u);
    CHECK_EQ(bus.read32(GPU_REG_MVP(3, 3)), 0u);
    CHECK_EQ(d.irq, 0);

    // ---- configuration outputs follow register writes
    bus.write32(GPU_REG_VBUF_BASE, 0x8000'1234);
    CHECK_EQ(bus.last_resp, OKAY);
    CHECK_EQ(d.vbuf_base, 0x8000'1230u);  // 16-byte aligned
    bus.write32(GPU_REG_VERTEX_COUNT, 36);
    CHECK_EQ(d.vertex_count, 36u);
    bus.write32(GPU_REG_RASTER_CFG, GPU_RASTER_CULL_BACK | GPU_RASTER_FRONT_CW);
    CHECK_EQ(d.cull_back_en, 1);
    CHECK_EQ(d.front_cw, 1);
    bus.write32(GPU_REG_CLEAR_COLOR, 0xABCD1234);
    CHECK_EQ(d.clear_color, 0x1234);
    bus.write32(GPU_REG_MVP(2, 1), 0xDEADBEEF);
    CHECK_EQ(static_cast<uint32_t>(d.mvp[2][1]), 0xDEADBEEFu);
    CHECK_EQ(bus.read32(GPU_REG_MVP(2, 1)), 0xDEADBEEFu);

    // ---- byte strobes
    bus.write(GPU_REG_VERTEX_COUNT, 0xAABBCCDD, 0x5);  // bytes 0 and 2
    CHECK_EQ(bus.read32(GPU_REG_VERTEX_COUNT), 0x00BB00DDu);
    bus.write(GPU_REG_VERTEX_COUNT, 0x11223344, 0x0);  // no strobes: no change
    CHECK_EQ(bus.read32(GPU_REG_VERTEX_COUNT), 0x00BB00DDu);

    // ---- unmapped addresses respond SLVERR, and writes to them change nothing
    for (uint32_t off : {0x24u, 0x3Cu, 0x60u, 0x7Cu, 0xC0u, 0xFCu}) {
        CHECK_EQ(bus.read32(off), 0u);
        CHECK_EQ(bus.last_resp, SLVERR);
        bus.write32(off, 0xFFFFFFFF);
        CHECK_EQ(bus.last_resp, SLVERR);
    }
    CHECK_EQ(bus.read32(GPU_REG_ID), GPU_ID_VALUE);

    // ---- read-only registers ignore writes
    bus.write32(GPU_REG_ID, 0);
    CHECK_EQ(bus.read32(GPU_REG_ID), GPU_ID_VALUE);

    // ---- command pulses; CTRL reads as zero
    bus.write32(GPU_REG_CTRL, GPU_CTRL_DRAW);
    bus.write32(GPU_REG_CTRL, GPU_CTRL_CLEAR | GPU_CTRL_DRAW);
    bus.write(GPU_REG_CTRL, GPU_CTRL_DRAW, 0x2);  // byte 0 not strobed: no command
    CHECK_EQ(pulses.draws, 2);
    CHECK_EQ(pulses.clears, 1);
    CHECK_EQ(bus.read32(GPU_REG_CTRL), 0u);

    // ---- STATUS: BUSY is live, DONE/FETCH_ERR are sticky and write-1-to-clear
    d.busy = 1;
    CHECK_EQ(bus.read32(GPU_REG_STATUS), GPU_STATUS_BUSY);
    d.busy = 0;
    d.done_set = 1;
    sim.tick();
    d.done_set = 0;
    CHECK_EQ(bus.read32(GPU_REG_STATUS), GPU_STATUS_DONE);
    bus.write32(GPU_REG_STATUS, GPU_STATUS_FETCH_ERR);  // wrong bit: DONE stays
    CHECK_EQ(bus.read32(GPU_REG_STATUS), GPU_STATUS_DONE);
    bus.write32(GPU_REG_STATUS, GPU_STATUS_DONE);
    CHECK_EQ(bus.read32(GPU_REG_STATUS), 0u);

    // A completion event on the same edge as a write-1-to-clear must not be
    // lost. Without jitter, AW/W are captured on edge 1 of a write and the
    // register update happens on edge 2. A pulse on edge 1 must be cleared by
    // the W1C (proves the alignment); a pulse on edge 2 must survive it.
    d.done_set = 1;  // pulse on edge 1 (the pulser zeroes it after the edge)
    bus.write32(GPU_REG_STATUS, GPU_STATUS_DONE);
    CHECK_EQ(bus.read32(GPU_REG_STATUS) & GPU_STATUS_DONE, 0u);
    pulser.countdown = 1;  // pulse on edge 2
    bus.write32(GPU_REG_STATUS, GPU_STATUS_DONE);
    CHECK_EQ(bus.read32(GPU_REG_STATUS) & GPU_STATUS_DONE, GPU_STATUS_DONE);
    bus.write32(GPU_REG_STATUS, GPU_STATUS_DONE);
    CHECK_EQ(bus.read32(GPU_REG_STATUS) & GPU_STATUS_DONE, 0u);

    // Issuing a command while idle clears a stale DONE; while busy it does not.
    bus.write32(GPU_REG_CTRL, GPU_CTRL_DRAW);
    CHECK_EQ(bus.read32(GPU_REG_STATUS), 0u);
    d.done_set = 1;
    sim.tick();
    d.done_set = 0;
    d.busy = 1;
    bus.write32(GPU_REG_CTRL, GPU_CTRL_DRAW);
    CHECK_EQ(bus.read32(GPU_REG_STATUS), GPU_STATUS_DONE | GPU_STATUS_BUSY);
    d.busy = 0;

    // ---- interrupt output
    CHECK_EQ(d.irq, 0);  // DONE set but not enabled
    bus.write32(GPU_REG_IRQ_EN, GPU_IRQ_DONE);
    CHECK_EQ(d.irq, 1);
    bus.write32(GPU_REG_STATUS, GPU_STATUS_DONE);
    CHECK_EQ(d.irq, 0);
    d.err_set = 1;
    sim.tick();
    d.err_set = 0;
    CHECK_EQ(d.irq, 0);  // FETCH_ERR not enabled
    CHECK_EQ(bus.read32(GPU_REG_STATUS), GPU_STATUS_FETCH_ERR);
    bus.write32(GPU_REG_IRQ_EN, GPU_IRQ_DONE | GPU_IRQ_FETCH_ERR);
    CHECK_EQ(d.irq, 1);
    bus.write32(GPU_REG_STATUS, GPU_STATUS_FETCH_ERR);
    CHECK_EQ(d.irq, 0);
    bus.write32(GPU_REG_IRQ_EN, 0);

    // ---- performance counter inputs are readable at their offsets
    for (uint32_t i = 0; i < GPU_NUM_PERF; ++i) d.perf[i] = 0x1000u * (i + 1) + i;
    for (uint32_t i = 0; i < GPU_NUM_PERF; ++i) {
        CHECK_EQ(bus.read32(GPU_REG_PERF_CYCLES + 4 * i), 0x1000u * (i + 1) + i);
        CHECK_EQ(bus.last_resp, OKAY);
    }

    // ---- random traffic with handshake jitter against the reference model
    bus.jitter = true;
    RefRegs ref;
    for (auto& [off, v] : ref.val) bus.write32(off, 0);
    std::vector<uint32_t> offsets;
    for (auto& [off, m] : ref.mask) offsets.push_back(off);
    const uint32_t extra[] = {GPU_REG_ID, GPU_REG_FB_INFO, GPU_REG_STATUS, 0x24, 0x64, 0xC4};
    for (int i = 0; i < 3000; ++i) {
        uint32_t off;
        if (sim.chance(85)) off = offsets[sim.rand_range(0, offsets.size() - 1)];
        else off = extra[sim.rand_range(0, 5)];
        if (sim.chance(50)) {
            const uint32_t data = sim.rand_u32();
            const uint32_t strb = sim.chance(70) ? 0xF : sim.rand_range(0, 15);
            bus.write(off, data, strb);
            if (ref.is_rw(off)) ref.write(off, data, strb);
            CHECK_EQ(bus.last_resp, is_mapped(off) ? OKAY : SLVERR);
        } else {
            const uint32_t got = bus.read32(off);
            CHECK_EQ(bus.last_resp, is_mapped(off) ? OKAY : SLVERR);
            if (ref.is_rw(off)) CHECK_MSG(got == ref.val[off], "offset 0x%02x read 0x%08x, model 0x%08x",
                                          off, got, ref.val[off]);
        }
    }
    // Configuration outputs match the model at the end.
    CHECK_EQ(d.vbuf_base, ref.val[GPU_REG_VBUF_BASE]);
    CHECK_EQ(d.vertex_count, ref.val[GPU_REG_VERTEX_COUNT]);
    CHECK_EQ(d.clear_color, ref.val[GPU_REG_CLEAR_COLOR]);
    for (int r = 0; r < 4; ++r)
        for (int c = 0; c < 4; ++c)
            CHECK_EQ(static_cast<uint32_t>(d.mvp[r][c]), ref.val[GPU_REG_MVP(r, c)]);

    return sim.finish();
}
