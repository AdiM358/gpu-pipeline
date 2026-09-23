// AXI4 read-only memory slave agent for DUTs with m_axi_ar* / m_axi_r* ports.
//
// - Accepts up to `max_outstanding` read bursts and returns them in order.
// - ARREADY and RVALID are randomised (ar_ready_pct / r_valid_pct) to create
//   back-pressure and gaps; RVALID, once raised, is held until RREADY.
// - Checks the master side of the protocol: ARVALID/ARADDR stable until
//   ARREADY, INCR bursts of 4-byte beats, no burst crossing a 4 KB boundary.
// - Error injection: any beat whose address is in [err_lo, err_hi) returns
//   SLVERR; `bad_rlast_burst` (n-th burst, 0-based) gets RLAST one beat early.
#pragma once

#include <algorithm>
#include <deque>
#include <vector>

#include "sim.h"

namespace tb {

template <class DUT>
class AxiMem : public Agent {
public:
    AxiMem(Sim<DUT>& sim, uint32_t base, size_t bytes)
        : sim_(sim), d_(*sim.dut), base_(base), words_(bytes / 4, 0) {}

    int ar_ready_pct = 100;
    int r_valid_pct = 100;
    size_t max_outstanding = 8;
    uint32_t err_lo = 0, err_hi = 0;
    int bad_rlast_burst = -1;

    // Statistics
    uint64_t bursts = 0, beats = 0;
    size_t peak_outstanding = 0;

    void write32(uint32_t addr, uint32_t v) { words_.at(index(addr)) = v; }
    uint32_t read32(uint32_t addr) const { return words_.at(index(addr)); }

    void reset_signals() {
        d_.m_axi_arready = 0;
        d_.m_axi_rvalid = 0;
        d_.m_axi_rdata = 0;
        d_.m_axi_rresp = 0;
        d_.m_axi_rlast = 0;
    }

    void observe() override {
        // Master must hold ARVALID and ARADDR until the handshake.
        if (ar_wait_) {
            CHECK_MSG(d_.m_axi_arvalid, "ARVALID dropped before ARREADY");
            CHECK_MSG(d_.m_axi_araddr == ar_wait_addr_, "ARADDR changed while waiting for ARREADY");
        }
        ar_wait_ = d_.m_axi_arvalid && !d_.m_axi_arready;
        ar_wait_addr_ = d_.m_axi_araddr;

        if (d_.m_axi_arvalid && d_.m_axi_arready) {
            Burst b{d_.m_axi_araddr, static_cast<uint32_t>(d_.m_axi_arlen) + 1, 0, bursts};
            CHECK_EQ(d_.m_axi_arsize, 2u);   // 4-byte beats
            CHECK_EQ(d_.m_axi_arburst, 1u);  // INCR
            CHECK_MSG((b.addr & 3) == 0, "unaligned ARADDR 0x%08x", b.addr);
            CHECK_MSG((b.addr >> 12) == ((b.addr + 4 * b.len - 1) >> 12),
                      "burst at 0x%08x len %u crosses a 4 KB boundary", b.addr, b.len);
            queue_.push_back(b);
            ++bursts;
            peak_outstanding = std::max(peak_outstanding, queue_.size());
        }
        r_hs_ = d_.m_axi_rvalid && d_.m_axi_rready;
        if (r_hs_) {
            ++beats;
            Burst& b = queue_.front();
            if (++b.beat == b.len) queue_.pop_front();
        }
    }

    void drive() override {
        d_.m_axi_arready = queue_.size() < max_outstanding && sim_.chance(ar_ready_pct);

        if (d_.m_axi_rvalid && !r_hs_) return;  // hold the presented beat
        if (queue_.empty() || !sim_.chance(r_valid_pct)) {
            d_.m_axi_rvalid = 0;
            d_.m_axi_rlast = 0;
            return;
        }
        const Burst& b = queue_.front();
        const uint32_t addr = b.addr + 4 * b.beat;
        const bool in_range = addr >= base_ && index(addr) < words_.size();
        const bool err = (addr >= err_lo && addr < err_hi) || !in_range;
        const uint32_t early = (static_cast<int64_t>(b.id) == bad_rlast_burst) ? 1 : 0;
        d_.m_axi_rvalid = 1;
        d_.m_axi_rdata = in_range ? words_[index(addr)] : 0xDEADBEEF;
        d_.m_axi_rresp = err ? 2 : 0;
        d_.m_axi_rlast = (b.beat + 1 + early == b.len) ? 1 : 0;
    }

    bool idle() const { return queue_.empty(); }

private:
    struct Burst {
        uint32_t addr, len, beat;
        uint64_t id;
    };
    size_t index(uint32_t addr) const { return (addr - base_) / 4; }

    Sim<DUT>& sim_;
    DUT& d_;
    uint32_t base_;
    std::vector<uint32_t> words_;
    std::deque<Burst> queue_;
    bool r_hs_ = false;
    bool ar_wait_ = false;
    uint32_t ar_wait_addr_ = 0;
};

}  // namespace tb
