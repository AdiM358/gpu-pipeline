// gpu_top system test. Everything goes through the C++ driver over AXI-Lite;
// frames are read back through the hardware read port and compared with the
// golden model pixel by pixel (colour and depth), along with every counter.
#include "gpu_system.h"

int main(int argc, char** argv) {
    tb::Sim<Vgpu_top> sim(argc, argv, "gpu_top");
    tb::GpuSystem sys(sim);
    sys.init();
    Vgpu_top& d = *sim.dut;
    CHECK_EQ(sys.gpu.id(), static_cast<uint32_t>(GPU_ID_VALUE));
    CHECK_EQ(sys.width, 320u);
    CHECK_EQ(sys.height, 240u);
    const double aspect = double(sys.width) / sys.height;

    // ---- CLEAR alone fills both buffers; it takes one cycle per pixel
    {
        scenes::Scene sc = scenes::single_triangle(aspect);
        sc.clear_color = 0xF81F;
        CHECK(sys.run(sc, true, false));
        model::Gpu ref(sys.cfg);
        ref.clear(sc.clear_color);
        sys.compare_frame(ref, "clear only");
        const uint32_t cycles = sys.gpu.counters().cycles;
        std::printf("  clear only: %u cycles for %u pixels\n", cycles, sys.width * sys.height);
        CHECK_MSG(cycles >= sys.width * sys.height && cycles < sys.width * sys.height + 16,
                  "clear took %u cycles", cycles);
    }

    // ---- regression scenes: CLEAR + DRAW, compared with the model
    sys.render_and_check(scenes::single_triangle(aspect));
    sys.render_and_check(scenes::overlap(aspect));
    sys.render_and_check(scenes::cube(aspect, 0.7));
    sys.render_and_check(scenes::clip_cases(aspect));
    for (uint64_t seed = 1; seed <= 6; ++seed) sys.render_and_check(scenes::random_tris(aspect, seed, 80));

    // ---- a second DRAW without CLEAR accumulates into the same buffers
    {
        const scenes::Scene a = scenes::cube(aspect, 0.2), b = scenes::overlap(aspect);
        CHECK(sys.run(a, true, true));
        CHECK(sys.run(b, false, true));
        model::Gpu ref(sys.cfg);
        ref.clear(a.clear_color);
        ref.draw(a.verts, a.mvp, a.cull_back, a.front_cw);
        ref.draw(b.verts, b.mvp, b.cull_back, b.front_cw);
        sys.compare_frame(ref, "draw without clear");
        sys.compare_counters(sys.gpu.counters(), ref.counters, "second draw");
    }

    // ---- random memory latency and back-pressure, jittered register bus:
    //      identical frames
    sys.mem.ar_ready_pct = 40;
    sys.mem.r_valid_pct = 50;
    sys.mem.latency = 24;
    sys.bus.jitter = true;
    sys.render_and_check(scenes::cube(aspect, 1.9));
    sys.render_and_check(scenes::random_tris(aspect, 99, 60));
    sys.mem.ar_ready_pct = sys.mem.r_valid_pct = 100;
    sys.mem.latency = 0;
    sys.bus.jitter = false;

    // ---- a command written while busy is ignored
    {
        const scenes::Scene sc = scenes::cube(aspect, 0.4);
        sys.load_vertices(sc.verts);
        sys.gpu.set_mvp_q16(sc.mvp);
        sys.gpu.set_vertex_buffer(tb::GpuSystem::kVramBase, static_cast<uint32_t>(sc.verts.size()));
        sys.gpu.start(true, true);
        CHECK(sys.gpu.status() & GPU_STATUS_BUSY);
        sys.gpu.start(false, true);  // ignored
        CHECK(sys.gpu.wait_done(5'000'000));
        const gpu::PerfCounters pc = sys.gpu.counters();
        CHECK_EQ(pc.verts, static_cast<uint32_t>(sc.verts.size()));  // one draw, not two
        for (int i = 0; i < 200; ++i) sim.tick();
        CHECK_EQ(sys.gpu.status() & (GPU_STATUS_BUSY | GPU_STATUS_DONE), 0u);
    }

    // ---- interrupt: DONE raises irq when enabled; acknowledging drops it
    {
        sys.gpu.set_irq_enable(GPU_IRQ_DONE);
        const scenes::Scene sc = scenes::single_triangle(aspect);
        sys.load_vertices(sc.verts);
        sys.gpu.start(false, true);
        int t = 0;
        while (!d.irq && t++ < 1'000'000) sim.tick();
        CHECK_EQ(d.irq, 1);
        CHECK(sys.gpu.status() & GPU_STATUS_DONE);
        CHECK(sys.gpu.wait_done(10));  // acknowledges DONE
        CHECK_EQ(d.irq, 0);
        sys.gpu.set_irq_enable(0);
    }

    // ---- bus error during vertex fetch: FETCH_ERR, the draw ends, and the
    //      next draw is unaffected
    {
        const scenes::Scene sc = scenes::random_tris(aspect, 5, 40);
        sys.load_vertices(sc.verts);
        sys.mem.err_lo = tb::GpuSystem::kVramBase + 16 * 50;
        sys.mem.err_hi = sys.mem.err_lo + 16;
        sys.gpu.set_vertex_buffer(tb::GpuSystem::kVramBase, static_cast<uint32_t>(sc.verts.size()));
        sys.gpu.start(true, true);
        uint32_t status = 0;
        CHECK(!sys.gpu.wait_done(5'000'000, &status));  // driver reports the error
        CHECK(status & GPU_STATUS_FETCH_ERR);
        CHECK(status & GPU_STATUS_DONE);
        CHECK_MSG(sys.gpu.counters().verts <= 50, "vertices past the error reached the pipeline");
        CHECK(sys.mem.idle());
        sys.mem.err_lo = sys.mem.err_hi = 0;
        CHECK_EQ(sys.gpu.status() & GPU_STATUS_FETCH_ERR, 0u);  // acknowledged by wait_done
        sys.render_and_check(scenes::cube(aspect, 2.5));
    }

    // ---- a draw whose vertex count is not a multiple of 3: the partial
    //      triangle is dropped and does not disturb the next draw
    {
        scenes::Scene sc = scenes::overlap(aspect);
        sc.verts.resize(7);
        sc.name = "partial_triangle";
        sys.render_and_check(sc);
        sys.render_and_check(scenes::overlap(aspect));
    }

    return sim.finish();
}
