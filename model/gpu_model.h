// Bit-accurate reference model of the GPU pipeline.
//
// This file is the numeric specification: every fixed-point format, rounding
// and wrap-around the RTL implements is written down here, and the testbenches
// require the RTL to match it exactly (vertex by vertex, triangle by triangle,
// pixel by pixel, counter by counter).
//
// It deliberately does NOT mirror the RTL's structure. The model evaluates
// each pixel's edge functions and attributes directly from the plane
// equations; the RTL steps them incrementally, spans several pixels per
// cycle, and pipelines everything. Agreement therefore checks the
// incremental/traversal logic, not just a transcription of it.
#pragma once

#include <cstdint>
#include <vector>

namespace model {

struct Config {
    int width = 320;   // pixels; must be <= 1023 (Q11.4 coordinates)
    int height = 240;
};

// ---------------------------------------------------------------- formats
// Clip-space vertex: Q16.16 x, y, z, w.
struct ClipVtx {
    int32_t x, y, z, w;
    uint32_t color;  // 0x00RRGGBB
    bool operator==(const ClipVtx&) const = default;
};

// Screen-space vertex after perspective divide and viewport transform.
enum : uint8_t {
    OC_XNEG = 1 << 0, OC_XPOS = 1 << 1, OC_YNEG = 1 << 2,
    OC_YPOS = 1 << 3, OC_ZNEG = 1 << 4, OC_ZPOS = 1 << 5,
    FLAG_NEAR = 1 << 6,  // w <= W_NEAR: behind or too close to the eye
    FLAG_GB = 1 << 7,    // screen position outside the guard band
};
struct ScreenVtx {
    int16_t sx, sy;   // Q11.4 pixels (1/16 pixel units), pixel centres at +8
    uint16_t z;       // depth, 0 = near, 0xFFFF = far
    uint32_t color;   // 0x00RRGGBB
    uint8_t flags;    // OC_* | FLAG_NEAR | FLAG_GB
    bool operator==(const ScreenVtx&) const = default;
};

constexpr int32_t kWNear = 0x1000;       // 1/16 in Q16.16
constexpr int32_t kGuard = 1 << 14;      // guard band: |sx|, |sy| < 1024 px
constexpr int kAttrFrac = 20;            // attribute accumulator fraction bits
constexpr int kAttrBits = 38;            // attribute accumulator width (wraps)
constexpr int kNumAttr = 4;              // z, r, g, b

// ---------------------------------------------------------------- stage 1
// MVP transform: clip = (M * [x y z 1]) >> 16, sums wrap at 48 bits
// (bits [47:16] are kept, exactly as the RTL's 32-bit result).
ClipVtx transform(const int32_t mvp[16], int32_t x, int32_t y, int32_t z, uint32_t color);

// ---------------------------------------------------------------- stage 2
// floor(2^44 / w) for w in (kWNear, 2^31); the pipelined divider's function.
uint32_t recip_w(int32_t w);
ScreenVtx persp_viewport(const ClipVtx& c, const Config& cfg);

// ---------------------------------------------------------------- stage 3
enum class TriFate { Raster, Culled, Clipped };

struct TriSetup {
    // Inclusive pixel bounding box, already clamped to the screen.
    int px0, px1, py0, py1;
    // Edge functions at the centre of pixel (px0, py0), fill-rule bias applied,
    // and their per-pixel steps in x and y.
    int64_t e0[3], ex[3], ey[3];
    // Attribute accumulators (z, r, g, b) at (px0, py0), per-pixel steps,
    // all kAttrBits wide with kAttrFrac fraction bits, and the clamp range.
    int64_t a0[kNumAttr], ax[kNumAttr], ay[kNumAttr];
    uint16_t amin[kNumAttr], amax[kNumAttr];
    bool operator==(const TriSetup&) const = default;
};

// Culling, rejection, edge equations and attribute gradients.
TriFate setup(const ScreenVtx v[3], bool cull_back, bool front_cw, const Config& cfg,
              TriSetup* out);

// Reciprocal used for gradients: floor(2^55 / (A << clz32(A))), A in [1, 2^31).
uint32_t recip_area(uint32_t area, int* lz);

// ---------------------------------------------------------------- stage 4
struct Fragment {
    uint16_t x, y, z;
    uint8_t r, g, b;
    bool operator==(const Fragment&) const = default;
};
// Fragments in the order the rasterizer emits them (row-major).
void rasterize(const TriSetup& t, std::vector<Fragment>& out);

// ---------------------------------------------------------------- stage 5
inline uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
    return static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

// ---------------------------------------------------------------- full pipeline
struct Vertex {
    int32_t x, y, z;  // Q16.16 model space
    uint32_t color;
};

struct Counters {
    uint32_t verts = 0, tri_in = 0, tri_culled = 0, tri_clipped = 0;
    uint32_t frag_gen = 0, frag_pass = 0;
};

class Gpu {
public:
    explicit Gpu(const Config& cfg = Config());

    // CTRL.CLEAR
    void clear(uint16_t color);
    // CTRL.DRAW: triangle list; a trailing partial triangle is dropped.
    void draw(const std::vector<Vertex>& verts, const int32_t mvp[16], bool cull_back,
              bool front_cw);

    const Config& config() const { return cfg_; }
    std::vector<uint16_t> color, depth;
    Counters counters;  // for the most recent draw

private:
    Config cfg_;
};

// Utilities for tests
int64_t wrap(int64_t v, int bits);  // sign-extend the low `bits` bits
int clz32(uint32_t v);

}  // namespace model
