// Full-system harness for Vgpu_top: AXI-Lite driver, AXI4 vertex memory,
// framebuffer readback through the hardware read port, and comparison with
// the golden model. Shared by the regression test and the demo renderer.
#pragma once

#include <cstdio>
#include <string>
#include <vector>

#include "Vgpu_top.h"
#include "axi_mem.h"
#include "axil_master.h"
#include "gpu_driver.hpp"
#include "gpu_model.h"
#include "scenes.h"
#include "sim.h"

namespace tb {

class GpuSystem {
public:
    using Dut = Vgpu_top;
    static constexpr uint32_t kVramBase = 0x8000'0000;
    static constexpr uint32_t kVramBytes = 1 << 20;

    explicit GpuSystem(Sim<Dut>& s) : sim(s), bus(s), mem(s, kVramBase, kVramBytes), gpu(bus) {
        s.add_agent(&mem);
        bus.idle();
        mem.reset_signals();
        s.dut->fb_rd_addr = 0;
        s.dut->fb_rd_depth = 0;
    }

    void init() {
        sim.reset();
        gpu.fb_size(width, height);
        cfg.width = static_cast<int>(width);
        cfg.height = static_cast<int>(height);
    }

    void load_vertices(const std::vector<model::Vertex>& v, uint32_t base = kVramBase) {
        for (size_t i = 0; i < v.size(); ++i) {
            const uint32_t a = base + 16 * static_cast<uint32_t>(i);
            mem.write32(a, v[i].x);
            mem.write32(a + 4, v[i].y);
            mem.write32(a + 8, v[i].z);
            mem.write32(a + 12, v[i].color);
        }
    }

    // Configures and runs one command for `sc`; returns false if the driver
    // reported a timeout or error.
    bool run(const scenes::Scene& sc, bool clear, bool draw, uint32_t* status = nullptr) {
        load_vertices(sc.verts);
        gpu.set_mvp_q16(sc.mvp);
        gpu.set_vertex_buffer(kVramBase, static_cast<uint32_t>(sc.verts.size()));
        gpu.set_cull(sc.cull_back, sc.front_cw);
        gpu.set_clear_color(sc.clear_color);
        gpu.start(clear, draw);
        return gpu.wait_done(5'000'000, status);
    }

    // Reads a whole buffer through the fb_rd port (2-cycle latency, one
    // address per cycle).
    std::vector<uint16_t> read_buffer(bool depth) {
        const size_t n = static_cast<size_t>(width) * height;
        std::vector<uint16_t> out;
        out.reserve(n);
        sim.dut->fb_rd_depth = depth;
        for (size_t i = 0; i < n + 2; ++i) {
            sim.dut->fb_rd_addr = static_cast<uint32_t>(i < n ? i : 0);
            sim.tick();
            if (i >= 1 && out.size() < n) out.push_back(sim.dut->fb_rd_data);
        }
        sim.dut->fb_rd_depth = 0;
        return out;
    }

    // Compares the hardware frame with `ref`; returns the number of
    // mismatching pixels (colour or depth).
    int compare_frame(const model::Gpu& ref, const std::string& what) {
        const auto c = read_buffer(false);
        const auto z = read_buffer(true);
        int bad = 0, first = -1;
        for (size_t i = 0; i < c.size(); ++i)
            if (c[i] != ref.color[i] || z[i] != ref.depth[i]) {
                if (first < 0) first = static_cast<int>(i);
                ++bad;
            }
        CHECK_MSG(bad == 0, "%s: %d pixels differ from the model (first at x=%u y=%u: rtl c=%04x z=%04x, "
                            "model c=%04x z=%04x)",
                  what.c_str(), bad, first % width, first / width, first >= 0 ? c[first] : 0,
                  first >= 0 ? z[first] : 0, first >= 0 ? ref.color[first] : 0, first >= 0 ? ref.depth[first] : 0);
        last_color = c;
        return bad;
    }

    void compare_counters(const gpu::PerfCounters& pc, const model::Counters& m, const std::string& what) {
        CHECK_MSG(pc.verts == m.verts && pc.tri_in == m.tri_in && pc.tri_culled == m.tri_culled &&
                      pc.tri_clipped == m.tri_clipped && pc.frag_gen == m.frag_gen && pc.frag_pass == m.frag_pass,
                  "%s: counters differ: rtl verts %u tri %u culled %u clipped %u gen %u pass %u; "
                  "model %u %u %u %u %u %u",
                  what.c_str(), pc.verts, pc.tri_in, pc.tri_culled, pc.tri_clipped, pc.frag_gen, pc.frag_pass,
                  m.verts, m.tri_in, m.tri_culled, m.tri_clipped, m.frag_gen, m.frag_pass);
    }

    // CLEAR+DRAW `sc` on the hardware and the model; compare everything.
    gpu::PerfCounters render_and_check(const scenes::Scene& sc, bool quiet = false) {
        uint32_t status = 0;
        CHECK_MSG(run(sc, true, true, &status), "%s: command failed (STATUS=0x%x)", sc.name.c_str(), status);
        const gpu::PerfCounters pc = gpu.counters();
        model::Gpu ref(cfg);
        ref.clear(sc.clear_color);
        ref.draw(sc.verts, sc.mvp, sc.cull_back, sc.front_cw);
        compare_frame(ref, sc.name);
        compare_counters(pc, ref.counters, sc.name);
        if (!quiet)
            std::printf("  %-16s tris %4u (culled %4u, clipped %3u)  frags %6u (pass %6u)  cycles %7u\n",
                        sc.name.c_str(), pc.tri_in, pc.tri_culled, pc.tri_clipped, pc.frag_gen, pc.frag_pass,
                        pc.cycles);
        return pc;
    }

    static bool write_ppm(const std::string& path, const std::vector<uint16_t>& rgb565, uint32_t w, uint32_t h) {
        FILE* f = std::fopen(path.c_str(), "wb");
        if (!f) return false;
        std::fprintf(f, "P6\n%u %u\n255\n", w, h);
        for (uint16_t p : rgb565) {
            const uint8_t r = static_cast<uint8_t>(((p >> 11) & 31) * 255 / 31);
            const uint8_t g = static_cast<uint8_t>(((p >> 5) & 63) * 255 / 63);
            const uint8_t b = static_cast<uint8_t>((p & 31) * 255 / 31);
            std::fputc(r, f), std::fputc(g, f), std::fputc(b, f);
        }
        std::fclose(f);
        return true;
    }

    Sim<Dut>& sim;
    AxilMaster<Dut> bus;
    AxiMem<Dut> mem;
    gpu::Driver gpu;
    uint32_t width = 0, height = 0;
    model::Config cfg;
    std::vector<uint16_t> last_color;
};

}  // namespace tb
