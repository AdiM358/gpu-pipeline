// Bit-accurate reference model. See gpu_model.h for the contract.
#include "gpu_model.h"

#include <algorithm>

namespace model {

int64_t wrap(int64_t v, int bits) {
    const uint64_t m = (bits == 64) ? ~0ULL : ((1ULL << bits) - 1);
    uint64_t u = static_cast<uint64_t>(v) & m;
    if (bits < 64 && (u >> (bits - 1)) & 1) u |= ~m;
    return static_cast<int64_t>(u);
}

int clz32(uint32_t v) {
    int n = 0;
    for (int b = 31; b >= 0 && !((v >> b) & 1); --b) ++n;
    return n;
}

// ---------------------------------------------------------------- transform
ClipVtx transform(const int32_t m[16], int32_t x, int32_t y, int32_t z, uint32_t color) {
    const int64_t v[4] = {x, y, z, 0x10000};
    int32_t r[4];
    for (int row = 0; row < 4; ++row) {
        uint64_t sum = 0;  // wraps like the RTL's 48-bit accumulation
        for (int c = 0; c < 4; ++c)
            sum += static_cast<uint64_t>(static_cast<int64_t>(m[4 * row + c]) * v[c]);
        r[row] = static_cast<int32_t>(static_cast<uint32_t>(sum >> 16));
    }
    return {r[0], r[1], r[2], r[3], color};
}

// ---------------------------------------------------------------- persp/viewport
uint32_t recip_w(int32_t w) {
    return static_cast<uint32_t>((1ULL << 44) / static_cast<uint64_t>(w));
}

ScreenVtx persp_viewport(const ClipVtx& c, const Config& cfg) {
    const int64_t x = c.x, y = c.y, z = c.z, w = c.w;
    uint8_t f = 0;
    if (x < -w) f |= OC_XNEG;
    if (x > w)  f |= OC_XPOS;
    if (y < -w) f |= OC_YNEG;
    if (y > w)  f |= OC_YPOS;
    if (z < -w) f |= OC_ZNEG;
    if (z > w)  f |= OC_ZPOS;
    const bool near = w <= kWNear;
    if (near) f |= FLAG_NEAR;

    // Near vertices divide by a harmless constant; the triangle is dropped.
    const int64_t recip = recip_w(near ? 2 * kWNear : c.w);
    const int64_t nx = (x * recip) >> 24;  // Q.20 NDC
    const int64_t ny = (y * recip) >> 24;
    const int64_t nz = (z * recip) >> 24;

    const int64_t hw = 8 * cfg.width;   // half width in 1/16 px
    const int64_t hh = 8 * cfg.height;
    const int64_t sx = (nx * hw + (hw << 20)) >> 20;
    const int64_t sy = ((hh << 20) - ny * hh) >> 20;
    if (sx < -kGuard || sx >= kGuard || sy < -kGuard || sy >= kGuard) f |= FLAG_GB;

    int64_t d = ((nz + (1LL << 20)) * 65535) >> 21;
    d = std::clamp<int64_t>(d, 0, 65535);

    return {static_cast<int16_t>(sx), static_cast<int16_t>(sy), static_cast<uint16_t>(d),
            c.color & 0xFFFFFF, f};
}

// ---------------------------------------------------------------- setup
uint32_t recip_area(uint32_t area, int* lz) {
    *lz = clz32(area);
    const uint64_t an = static_cast<uint64_t>(area) << *lz;  // [2^31, 2^32)
    return static_cast<uint32_t>((1ULL << 55) / an);          // (2^23, 2^24]
}

namespace {
uint16_t attr(const ScreenVtx& v, int i) {
    switch (i) {
        case 0: return v.z;
        case 1: return (v.color >> 16) & 0xFF;
        case 2: return (v.color >> 8) & 0xFF;
        default: return v.color & 0xFF;
    }
}
}  // namespace

TriFate setup(const ScreenVtx in[3], bool cull_back, bool front_cw, const Config& cfg,
              TriSetup* out) {
    // Trivial reject (all vertices outside one frustum plane), near plane,
    // guard band.
    if ((in[0].flags & in[1].flags & in[2].flags & 0x3F) ||
        ((in[0].flags | in[1].flags | in[2].flags) & (FLAG_NEAR | FLAG_GB)))
        return TriFate::Clipped;

    // Signed doubled area in screen space (y down). Counter-clockwise in NDC
    // (y up) is negative here.
    int64_t dx1 = in[1].sx - in[0].sx, dy1 = in[1].sy - in[0].sy;
    int64_t dx2 = in[2].sx - in[0].sx, dy2 = in[2].sy - in[0].sy;
    int64_t area = dx1 * dy2 - dx2 * dy1;
    const bool front = front_cw ? (area > 0) : (area < 0);
    if (area == 0 || (cull_back && !front)) return TriFate::Culled;

    // Bounding box in pixels whose centres (16*p + 8) lie inside it.
    const int xmin = std::min({in[0].sx, in[1].sx, in[2].sx});
    const int xmax = std::max({in[0].sx, in[1].sx, in[2].sx});
    const int ymin = std::min({in[0].sy, in[1].sy, in[2].sy});
    const int ymax = std::max({in[0].sy, in[1].sy, in[2].sy});
    TriSetup t{};
    t.px0 = std::max((xmin + 7) >> 4, 0);
    t.px1 = std::min((xmax - 8) >> 4, cfg.width - 1);
    t.py0 = std::max((ymin + 7) >> 4, 0);
    t.py1 = std::min((ymax - 8) >> 4, cfg.height - 1);
    if (t.px0 > t.px1 || t.py0 > t.py1) return TriFate::Clipped;  // no pixel centres

    // Make the winding positive (swap v1/v2) so inside means all E >= 0.
    ScreenVtx v[3] = {in[0], in[1], in[2]};
    if (area < 0) {
        std::swap(v[1], v[2]);
        std::swap(dx1, dx2);
        std::swap(dy1, dy2);
        area = -area;
    }

    const int64_t cx = 16 * t.px0 + 8, cy = 16 * t.py0 + 8;
    for (int e = 0; e < 3; ++e) {
        const ScreenVtx& a = v[e];
        const ScreenVtx& b = v[(e + 1) % 3];
        const int64_t dxe = b.sx - a.sx, dye = b.sy - a.sy;
        // Top-left rule: an edge owns its boundary pixels if it is a left edge
        // (dy < 0) or a top edge (dy == 0, dx > 0).
        const bool top_left = dye < 0 || (dye == 0 && dxe > 0);
        t.e0[e] = (cy - a.sy) * dxe - (cx - a.sx) * dye - (top_left ? 0 : 1);
        t.ex[e] = -16 * dye;
        t.ey[e] = 16 * dxe;
    }

    int lz;
    const int64_t r = recip_area(static_cast<uint32_t>(area), &lz);
    const int sh = 35 - lz;
    for (int i = 0; i < kNumAttr; ++i) {
        const int64_t a0 = attr(v[0], i), a1 = attr(v[1], i), a2 = attr(v[2], i);
        const int64_t da1 = a1 - a0, da2 = a2 - a0;
        const int64_t nx = da1 * dy2 - da2 * dy1;
        const int64_t ny = da2 * dx1 - da1 * dx2;
        const int64_t gx = wrap((nx * r) >> sh, kAttrBits);  // per 1/16 px, Q.20
        const int64_t gy = wrap((ny * r) >> sh, kAttrBits);
        t.a0[i] = wrap((a0 << kAttrFrac) + gx * (cx - v[0].sx) + gy * (cy - v[0].sy), kAttrBits);
        t.ax[i] = wrap(gx * 16, kAttrBits);
        t.ay[i] = wrap(gy * 16, kAttrBits);
        t.amin[i] = static_cast<uint16_t>(std::min({a0, a1, a2}));
        t.amax[i] = static_cast<uint16_t>(std::max({a0, a1, a2}));
    }
    *out = t;
    return TriFate::Raster;
}

// ---------------------------------------------------------------- raster
void rasterize(const TriSetup& t, std::vector<Fragment>& out) {
    for (int py = t.py0; py <= t.py1; ++py) {
        for (int px = t.px0; px <= t.px1; ++px) {
            const int64_t dx = px - t.px0, dy = py - t.py0;
            bool inside = true;
            for (int e = 0; e < 3; ++e) inside &= (t.e0[e] + dx * t.ex[e] + dy * t.ey[e]) >= 0;
            if (!inside) continue;
            uint16_t val[kNumAttr];
            for (int i = 0; i < kNumAttr; ++i) {
                const int64_t acc = wrap(t.a0[i] + dx * t.ax[i] + dy * t.ay[i], kAttrBits);
                const int64_t rounded = wrap(acc + (1LL << (kAttrFrac - 1)), kAttrBits) >> kAttrFrac;
                val[i] = static_cast<uint16_t>(
                    std::clamp<int64_t>(rounded, t.amin[i], t.amax[i]));
            }
            out.push_back({static_cast<uint16_t>(px), static_cast<uint16_t>(py), val[0],
                           static_cast<uint8_t>(val[1]), static_cast<uint8_t>(val[2]),
                           static_cast<uint8_t>(val[3])});
        }
    }
}

// ---------------------------------------------------------------- full pipeline
Gpu::Gpu(const Config& cfg)
    : color(cfg.width * cfg.height, 0), depth(cfg.width * cfg.height, 0xFFFF), cfg_(cfg) {}

void Gpu::clear(uint16_t c) {
    std::fill(color.begin(), color.end(), c);
    std::fill(depth.begin(), depth.end(), 0xFFFF);
}

void Gpu::draw(const std::vector<Vertex>& verts, const int32_t mvp[16], bool cull_back,
               bool front_cw) {
    counters = Counters{};
    counters.verts = static_cast<uint32_t>(verts.size());
    std::vector<Fragment> frags;
    for (size_t i = 0; i + 2 < verts.size(); i += 3) {
        ScreenVtx s[3];
        for (int k = 0; k < 3; ++k) {
            const Vertex& v = verts[i + k];
            s[k] = persp_viewport(transform(mvp, v.x, v.y, v.z, v.color), cfg_);
        }
        ++counters.tri_in;
        TriSetup t;
        switch (setup(s, cull_back, front_cw, cfg_, &t)) {
            case TriFate::Culled: ++counters.tri_culled; continue;
            case TriFate::Clipped: ++counters.tri_clipped; continue;
            case TriFate::Raster: break;
        }
        frags.clear();
        rasterize(t, frags);
        for (const Fragment& f : frags) {
            ++counters.frag_gen;
            const size_t a = static_cast<size_t>(f.y) * cfg_.width + f.x;
            if (f.z < depth[a]) {
                depth[a] = f.z;
                color[a] = rgb565(f.r, f.g, f.b);
                ++counters.frag_pass;
            }
        }
    }
}

}  // namespace model
