// Minimal C++ driver for the GPU register interface.
//
// The driver only needs 32-bit register reads and writes, supplied through
// the RegBus interface. In this repository the only RegBus implementation is
// the Verilator testbench's AXI4-Lite master (tb/common/axil_master.h); a
// Linux /dev/mem or bare-metal Xil_Out32 implementation would be a few lines,
// but none is provided because no hardware was available to test it on.
#pragma once

#include <cmath>
#include <cstdint>

#include "gpu_regs.h"

namespace gpu {

class RegBus {
public:
    virtual ~RegBus() = default;
    virtual uint32_t read32(uint32_t offset) = 0;
    virtual void write32(uint32_t offset, uint32_t value) = 0;
};

// Round-to-nearest conversion to Q16.16 with saturation.
inline int32_t to_q16(double v) {
    const double s = std::nearbyint(v * 65536.0);
    if (s >= 2147483647.0) return INT32_MAX;
    if (s <= -2147483648.0) return INT32_MIN;
    return static_cast<int32_t>(s);
}

struct PerfCounters {
    uint32_t cycles, verts, tri_in, tri_culled, tri_clipped, frag_gen, frag_pass, rast_busy;
};

class Driver {
public:
    explicit Driver(RegBus& bus) : bus_(bus) {}

    uint32_t id() { return bus_.read32(GPU_REG_ID); }
    uint32_t status() { return bus_.read32(GPU_REG_STATUS); }
    void fb_size(uint32_t& w, uint32_t& h) {
        const uint32_t v = bus_.read32(GPU_REG_FB_INFO);
        w = v & 0xFFFF;
        h = v >> 16;
    }

    // Row-major 4x4 matrix, already in Q16.16.
    void set_mvp_q16(const int32_t m[16]) {
        for (uint32_t i = 0; i < 16; ++i)
            bus_.write32(GPU_REG_MVP0 + 4 * i, static_cast<uint32_t>(m[i]));
    }
    void set_mvp(const double m[16]) {
        int32_t q[16];
        for (int i = 0; i < 16; ++i) q[i] = to_q16(m[i]);
        set_mvp_q16(q);
    }

    void set_vertex_buffer(uint32_t base_addr, uint32_t vertex_count) {
        bus_.write32(GPU_REG_VBUF_BASE, base_addr);
        bus_.write32(GPU_REG_VERTEX_COUNT, vertex_count);
    }
    void set_cull(bool cull_back, bool front_cw = false) {
        bus_.write32(GPU_REG_RASTER_CFG,
                     (cull_back ? GPU_RASTER_CULL_BACK : 0u) | (front_cw ? GPU_RASTER_FRONT_CW : 0u));
    }
    void set_clear_color(uint16_t rgb565) { bus_.write32(GPU_REG_CLEAR_COLOR, rgb565); }
    void set_irq_enable(uint32_t mask) { bus_.write32(GPU_REG_IRQ_EN, mask); }

    // Issues a command. A command written while BUSY is ignored by hardware.
    void start(bool clear, bool draw) {
        bus_.write32(GPU_REG_CTRL, (clear ? GPU_CTRL_CLEAR : 0u) | (draw ? GPU_CTRL_DRAW : 0u));
    }

    // Polls STATUS until DONE is set and BUSY is clear, then acknowledges DONE.
    // Returns false on timeout or if a fetch error was reported.
    bool wait_done(uint32_t max_polls, uint32_t* final_status = nullptr) {
        for (uint32_t i = 0; i < max_polls; ++i) {
            const uint32_t s = status();
            if ((s & GPU_STATUS_DONE) && !(s & GPU_STATUS_BUSY)) {
                bus_.write32(GPU_REG_STATUS, GPU_STATUS_DONE | (s & GPU_STATUS_FETCH_ERR));
                if (final_status) *final_status = s;
                return !(s & GPU_STATUS_FETCH_ERR);
            }
        }
        if (final_status) *final_status = status();
        return false;
    }

    PerfCounters counters() {
        PerfCounters p{};
        p.cycles      = bus_.read32(GPU_REG_PERF_CYCLES);
        p.verts       = bus_.read32(GPU_REG_PERF_VERTS);
        p.tri_in      = bus_.read32(GPU_REG_PERF_TRI_IN);
        p.tri_culled  = bus_.read32(GPU_REG_PERF_TRI_CULLED);
        p.tri_clipped = bus_.read32(GPU_REG_PERF_TRI_CLIPPED);
        p.frag_gen    = bus_.read32(GPU_REG_PERF_FRAG_GEN);
        p.frag_pass   = bus_.read32(GPU_REG_PERF_FRAG_PASS);
        p.rast_busy   = bus_.read32(GPU_REG_PERF_RAST_BUSY);
        return p;
    }

private:
    RegBus& bus_;
};

}  // namespace gpu
