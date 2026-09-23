// System-level performance benchmark. Built once per RAST_SPAN value (see
// `make perf`). Each scene is drawn with a DRAW-only command after a separate
// CLEAR, so PERF_CYCLES is the draw alone. Results are also checked against
// the golden model, so every span width is verified end to end.
//
//   +span=N   printed label only (the RTL parameter is set at build time)
#include <cstdlib>

#include "gpu_system.h"

int main(int argc, char** argv) {
    tb::Sim<Vgpu_top> sim(argc, argv, "perf");
    tb::GpuSystem sys(sim);
    sys.init();
    const char* sp = sim.ctx()->commandArgsPlusMatch("span=");
    const int span = (sp && *sp) ? std::atoi(sp + 6) : 0;
    const double aspect = double(sys.width) / sys.height;

    std::vector<scenes::Scene> bench = {scenes::demo_frame(aspect, 0.0)[0], scenes::random_tris(aspect, 3, 200),
                                        scenes::random_tris(aspect, 8, 200)};
    for (const scenes::Scene& sc : bench) {
        CHECK(sys.run(sc, true, false));  // clear only
        CHECK(sys.run(sc, false, true));  // draw only: PERF_CYCLES excludes the clear
        const gpu::PerfCounters pc = sys.gpu.counters();
        model::Gpu ref(sys.cfg);
        ref.clear(sc.clear_color);
        ref.draw(sc.verts, sc.mvp, sc.cull_back, sc.front_cw);
        sys.compare_frame(ref, sc.name);
        sys.compare_counters(pc, ref.counters, sc.name);
        std::printf("| %d | %s | %u | %u | %u | %u | %.3f | %u |\n", span, sc.name.c_str(), pc.tri_in,
                    pc.tri_in - pc.tri_culled - pc.tri_clipped, pc.frag_gen, pc.cycles,
                    double(pc.frag_gen) / pc.cycles, pc.rast_busy);
    }
    return sim.finish();
}
