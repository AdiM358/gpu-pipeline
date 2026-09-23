// Blocking AXI4-Lite master BFM for any DUT with s_axi_* ports.
//
// Implements gpu::RegBus, so the C++ driver in sw/ runs unchanged on top of
// it. With jitter enabled, AWVALID/WVALID are raised after independent random
// delays (so AW-before-W, W-before-AW and simultaneous all occur) and
// BREADY/RREADY are delayed randomly to exercise slave back-pressure.
#pragma once

#include "gpu_driver.hpp"
#include "sim.h"

namespace tb {

template <class DUT>
class AxilMaster : public gpu::RegBus {
public:
    explicit AxilMaster(Sim<DUT>& sim) : sim_(sim) {}

    bool jitter = false;
    int timeout_cycles = 200;
    uint32_t last_resp = 0;  // BRESP/RRESP of the most recent access
    uint64_t writes = 0, reads = 0;

    void idle() {
        auto& d = *sim_.dut;
        d.s_axi_awvalid = 0;
        d.s_axi_wvalid = 0;
        d.s_axi_bready = 0;
        d.s_axi_arvalid = 0;
        d.s_axi_rready = 0;
        d.s_axi_wstrb = 0xF;
    }

    void write32(uint32_t offset, uint32_t value) override { write(offset, value, 0xF); }

    void write(uint32_t offset, uint32_t value, uint32_t strb) {
        auto& d = *sim_.dut;
        int aw_delay = jitter ? sim_.rand_range(0, 3) : 0;
        int w_delay = jitter ? sim_.rand_range(0, 3) : 0;
        int b_delay = jitter ? sim_.rand_range(0, 3) : 0;
        bool aw_done = false, w_done = false, b_done = false;
        d.s_axi_awaddr = offset;
        d.s_axi_wdata = value;
        d.s_axi_wstrb = strb;
        for (int t = 0; !(aw_done && w_done && b_done); ++t) {
            if (t > timeout_cycles) FATAL("AXI-Lite write to 0x%02x timed out", offset);
            d.s_axi_awvalid = !aw_done && aw_delay-- <= 0;
            d.s_axi_wvalid = !w_done && w_delay-- <= 0;
            d.s_axi_bready = aw_done && w_done && b_delay-- <= 0;
            sim_.settle();
            const bool aw_hs = d.s_axi_awvalid && d.s_axi_awready;
            const bool w_hs = d.s_axi_wvalid && d.s_axi_wready;
            const bool b_hs = d.s_axi_bvalid && d.s_axi_bready;
            // AXI: a response must not precede the address/data handshakes.
            CHECK_MSG(!(d.s_axi_bvalid && !(aw_done && w_done)),
                      "BVALID before AW/W handshakes (offset 0x%02x)", offset);
            if (b_hs) last_resp = d.s_axi_bresp;
            sim_.tick();
            aw_done |= aw_hs;
            w_done |= w_hs;
            b_done |= b_hs;
        }
        d.s_axi_awvalid = d.s_axi_wvalid = d.s_axi_bready = 0;
        ++writes;
    }

    uint32_t read32(uint32_t offset) override {
        auto& d = *sim_.dut;
        int r_delay = jitter ? sim_.rand_range(0, 3) : 0;
        bool ar_done = false;
        uint32_t data = 0;
        d.s_axi_araddr = offset;
        for (int t = 0;; ++t) {
            if (t > timeout_cycles) FATAL("AXI-Lite read of 0x%02x timed out", offset);
            d.s_axi_arvalid = !ar_done;
            d.s_axi_rready = ar_done && r_delay-- <= 0;
            sim_.settle();
            const bool ar_hs = d.s_axi_arvalid && d.s_axi_arready;
            const bool r_hs = d.s_axi_rvalid && d.s_axi_rready;
            if (r_hs) {
                data = d.s_axi_rdata;
                last_resp = d.s_axi_rresp;
            }
            sim_.tick();
            ar_done |= ar_hs;
            if (r_hs) break;
        }
        d.s_axi_arvalid = d.s_axi_rready = 0;
        ++reads;
        return data;
    }

private:
    Sim<DUT>& sim_;
};

}  // namespace tb
