// Demo renderer: an animated scene (rotating cube + orbiting cube) drawn
// through the C++ driver, every frame checked against the golden model and
// written as a PPM. Frame cycle counts come from the PERF_CYCLES register.
//
//   +frames=N   number of frames (default 48)
//   +out=DIR    directory for frame_NNN.ppm (default: current directory)
#include <algorithm>
#include <cstdlib>

#include "gpu_system.h"

int main(int argc, char** argv) {
    tb::Sim<Vgpu_top> sim(argc, argv, "demo");
    tb::GpuSystem sys(sim);
    sys.init();

    const char* f = sim.ctx()->commandArgsPlusMatch("frames=");
    const int frames = (f && *f) ? std::atoi(f + 8) : 48;
    const char* o = sim.ctx()->commandArgsPlusMatch("out=");
    const std::string out = (o && *o) ? std::string(o + 5) : ".";
    const double aspect = double(sys.width) / sys.height;

    uint64_t total = 0, min_c = ~0ull, max_c = 0, frags = 0, tris = 0, culled = 0;
    for (int i = 0; i < frames; ++i) {
        const auto objs = scenes::demo_frame(aspect, double(i) / frames);
        model::Gpu ref(sys.cfg);
        uint64_t cycles = 0;
        for (size_t k = 0; k < objs.size(); ++k) {
            const bool clear = (k == 0);
            uint32_t status = 0;
            CHECK_MSG(sys.run(objs[k], clear, true, &status), "frame %d draw %zu failed (0x%x)", i, k, status);
            const gpu::PerfCounters pc = sys.gpu.counters();
            if (clear) ref.clear(objs[k].clear_color);
            ref.draw(objs[k].verts, objs[k].mvp, objs[k].cull_back, objs[k].front_cw);
            sys.compare_counters(pc, ref.counters, objs[k].name);
            cycles += pc.cycles;
            frags += pc.frag_gen;
            tris += pc.tri_in;
            culled += pc.tri_culled;
        }
        sys.compare_frame(ref, "frame " + std::to_string(i));
        char path[512];
        std::snprintf(path, sizeof path, "%s/frame_%03d.ppm", out.c_str(), i);
        CHECK_MSG(tb::GpuSystem::write_ppm(path, sys.last_color, sys.width, sys.height), "cannot write %s", path);
        total += cycles;
        min_c = std::min(min_c, cycles);
        max_c = std::max(max_c, cycles);
    }
    std::printf("  %d frames, %llu triangles submitted (%llu culled), %llu fragments\n", frames,
                (unsigned long long)tris, (unsigned long long)culled, (unsigned long long)frags);
    std::printf("  cycles/frame (clear + both draws, from PERF_CYCLES): mean %.0f, min %llu, max %llu\n",
                double(total) / frames, (unsigned long long)min_c, (unsigned long long)max_c);
    return sim.finish();
}
