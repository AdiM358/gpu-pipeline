// Test and demo scenes. Each scene is a triangle list plus a Q16.16 MVP
// matrix; the same integer matrix and vertices go to the RTL (through the
// driver) and to the golden model, so any difference is the hardware's.
#pragma once

#include <cmath>
#include <random>
#include <string>
#include <vector>

#include "gpu_driver.hpp"
#include "gpu_model.h"

namespace scenes {

// ---------------------------------------------------------------- 4x4 math (row-major)
struct Mat4 {
    double m[16];
    static Mat4 identity() { return {{1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
    Mat4 operator*(const Mat4& o) const {
        Mat4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k) r.m[4 * i + j] += m[4 * i + k] * o.m[4 * k + j];
        return r;
    }
};
inline Mat4 translate(double x, double y, double z) {
    Mat4 r = Mat4::identity();
    r.m[3] = x, r.m[7] = y, r.m[11] = z;
    return r;
}
inline Mat4 scale(double s) {
    Mat4 r = Mat4::identity();
    r.m[0] = r.m[5] = r.m[10] = s;
    return r;
}
inline Mat4 rot_x(double a) { return {{1, 0, 0, 0, 0, cos(a), -sin(a), 0, 0, sin(a), cos(a), 0, 0, 0, 0, 1}}; }
inline Mat4 rot_y(double a) { return {{cos(a), 0, sin(a), 0, 0, 1, 0, 0, -sin(a), 0, cos(a), 0, 0, 0, 0, 1}}; }
inline Mat4 rot_z(double a) { return {{cos(a), -sin(a), 0, 0, sin(a), cos(a), 0, 0, 0, 0, 1, 0, 0, 0, 0, 1}}; }
// OpenGL-style projection: camera at the origin looking down -z, clip w = -z_eye.
inline Mat4 perspective(double fovy_deg, double aspect, double n, double f) {
    const double t = 1.0 / std::tan(fovy_deg * M_PI / 360.0);
    return {{t / aspect, 0, 0, 0, 0, t, 0, 0, 0, 0, (f + n) / (n - f), 2 * f * n / (n - f), 0, 0, -1, 0}};
}

struct Scene {
    std::string name;
    std::vector<model::Vertex> verts;
    int32_t mvp[16];
    bool cull_back = false, front_cw = false;
    uint16_t clear_color = 0;

    void set_mvp(const Mat4& m) {
        for (int i = 0; i < 16; ++i) mvp[i] = gpu::to_q16(m.m[i]);
    }
    void tri(const double a[3], const double b[3], const double c[3], uint32_t ca, uint32_t cb, uint32_t cc) {
        verts.push_back({gpu::to_q16(a[0]), gpu::to_q16(a[1]), gpu::to_q16(a[2]), ca});
        verts.push_back({gpu::to_q16(b[0]), gpu::to_q16(b[1]), gpu::to_q16(b[2]), cb});
        verts.push_back({gpu::to_q16(c[0]), gpu::to_q16(c[1]), gpu::to_q16(c[2]), cc});
    }
};

inline Mat4 camera(double aspect) { return perspective(60.0, aspect, 0.5, 20.0) * translate(0, 0, -3.0); }

// ---------------------------------------------------------------- builders
inline Scene single_triangle(double aspect) {
    Scene s;
    s.name = "single_triangle";
    const double a[3] = {-0.9, -0.7, 0}, b[3] = {0.9, -0.5, 0}, c[3] = {0.1, 0.8, 0};
    s.tri(a, b, c, 0xFF0000, 0x00FF00, 0x0000FF);
    s.set_mvp(camera(aspect));
    s.clear_color = 0x0841;
    return s;
}

// Two triangles that pierce each other plus one in front of both: the
// visible surface changes along the intersection line, which only works
// with per-pixel interpolated depth.
inline Scene overlap(double aspect) {
    Scene s;
    s.name = "overlap";
    const double a0[3] = {-1.0, -0.6, -0.6}, a1[3] = {1.0, -0.6, 0.6}, a2[3] = {0.0, 0.9, 0.0};
    const double b0[3] = {-1.0, -0.4, 0.6}, b1[3] = {1.0, -0.4, -0.6}, b2[3] = {0.0, 0.8, 0.0};
    const double c0[3] = {-0.3, -0.9, 0.9}, c1[3] = {0.4, -0.9, 0.9}, c2[3] = {0.05, -0.1, 0.9};
    s.tri(a0, a1, a2, 0xFF4040, 0xFF4040, 0xFFC0C0);
    s.tri(b0, b1, b2, 0x4040FF, 0x4040FF, 0xC0C0FF);
    s.tri(c0, c1, c2, 0x40FF40, 0x40FF40, 0x40FF40);
    s.set_mvp(camera(aspect) * rot_y(0.3));
    return s;
}

// Unit cube, outward faces counter-clockwise, one colour per corner
// (Gouraud-shaded), back faces culled.
inline void add_cube(Scene& s, double size) {
    const double h = size / 2;
    const double p[8][3] = {{-h, -h, -h}, {h, -h, -h}, {h, h, -h}, {-h, h, -h},
                            {-h, -h, h},  {h, -h, h},  {h, h, h},  {-h, h, h}};
    const uint32_t col[8] = {0x202020, 0xFF2020, 0xFFFF20, 0x20FF20, 0x2020FF, 0xFF20FF, 0xFFFFFF, 0x20FFFF};
    const int faces[6][4] = {{4, 5, 6, 7}, {1, 0, 3, 2}, {5, 1, 2, 6}, {0, 4, 7, 3}, {7, 6, 2, 3}, {0, 1, 5, 4}};
    for (const auto& f : faces) {
        s.tri(p[f[0]], p[f[1]], p[f[2]], col[f[0]], col[f[1]], col[f[2]]);
        s.tri(p[f[0]], p[f[2]], p[f[3]], col[f[0]], col[f[2]], col[f[3]]);
    }
}

inline Scene cube(double aspect, double angle) {
    Scene s;
    s.name = "cube";
    add_cube(s, 1.4);
    s.set_mvp(camera(aspect) * rot_y(angle) * rot_x(0.6 + 0.5 * angle));
    s.cull_back = true;
    s.clear_color = gpu_rgb565(16, 16, 40);
    return s;
}

// Demo frame t in [0, 1): a large rotating cube and a small cube orbiting
// through it. Two draws (CLEAR+DRAW, then DRAW) share one depth buffer, so
// the intersection is resolved per pixel.
inline std::vector<Scene> demo_frame(double aspect, double t) {
    const double a = 2.0 * M_PI * t;
    Scene big;
    big.name = "demo_big";
    add_cube(big, 1.3);
    big.set_mvp(camera(aspect) * rot_y(a) * rot_x(0.5 + 0.35 * std::sin(a)));
    big.cull_back = true;
    big.clear_color = gpu_rgb565(16, 16, 40);
    Scene small;
    small.name = "demo_small";
    add_cube(small, 0.7);
    small.set_mvp(camera(aspect) * translate(0.95 * std::cos(2 * a), 0.25 * std::sin(a), 0.95 * std::sin(2 * a)) *
                  rot_x(3 * a) * rot_z(2 * a));
    small.cull_back = true;
    return {big, small};
}

// Random triangles in and around the view volume: some behind the camera,
// some off-screen, some huge, both windings; culling on or off.
inline Scene random_tris(double aspect, uint64_t seed, int n) {
    Scene s;
    s.name = "random_" + std::to_string(seed);
    std::mt19937_64 rng(seed);
    std::uniform_real_distribution<double> u(-1.0, 1.0);
    for (int i = 0; i < n; ++i) {
        const double cx = 1.5 * u(rng), cy = 1.2 * u(rng), cz = 2.2 * u(rng);
        const double r = (i % 10 == 0) ? 2.5 : 0.1 + 0.5 * std::abs(u(rng));
        double v[3][3];
        uint32_t c[3];
        for (int k = 0; k < 3; ++k) {
            v[k][0] = cx + r * u(rng);
            v[k][1] = cy + r * u(rng);
            v[k][2] = cz + r * u(rng);
            c[k] = static_cast<uint32_t>(rng()) & 0xFFFFFF;
        }
        s.tri(v[0], v[1], v[2], c[0], c[1], c[2]);
    }
    s.set_mvp(camera(aspect) * rot_x(0.2 * u(rng)));
    s.cull_back = (seed & 1) != 0;
    s.front_cw = (seed & 2) != 0;
    s.clear_color = static_cast<uint16_t>(rng());
    return s;
}

// Clipping corner cases: a triangle crossing the near plane (dropped), one
// behind the camera, one outside each side plane, one beyond the far plane,
// one partly visible and one large enough to leave the guard band.
inline Scene clip_cases(double aspect) {
    Scene s;
    s.name = "clip_cases";
    auto t = [&](double x0, double y0, double z0, double x1, double y1, double z1, double x2, double y2, double z2,
                 uint32_t c) {
        const double a[3] = {x0, y0, z0}, b[3] = {x1, y1, z1}, d[3] = {x2, y2, z2};
        s.tri(a, b, d, c, c, c);
    };
    t(-0.5, -0.5, 0.0, 0.5, -0.5, 0.0, 0.0, 0.5, 3.5, 0xFF0000);    // crosses the near plane
    t(-0.5, -0.5, 4.0, 0.5, -0.5, 4.0, 0.0, 0.5, 4.0, 0x00FF00);    // behind the camera
    t(5.0, 0.0, 0.0, 6.0, 0.0, 0.0, 5.5, 1.0, 0.0, 0x0000FF);       // right of the frustum
    t(-6.0, 0.0, 0.0, -5.0, 0.0, 0.0, -5.5, 1.0, 0.0, 0x0000FF);    // left
    t(0.0, 5.0, 0.0, 1.0, 5.0, 0.0, 0.5, 6.0, 0.0, 0x0000FF);       // above
    t(0.0, -6.0, 0.0, 1.0, -6.0, 0.0, 0.5, -5.0, 0.0, 0x0000FF);    // below
    t(0.0, 0.0, -40.0, 1.0, 0.0, -40.0, 0.5, 1.0, -40.0, 0xFFFF00); // beyond far
    t(-1.5, -0.4, 0.0, 0.8, -0.8, 0.2, 0.2, 2.5, -0.2, 0xFF00FF);   // partly off-screen: drawn
    t(-40.0, -1.0, -1.0, 40.0, -1.0, -1.0, 0.0, 30.0, -1.0, 0x00FFFF); // leaves the guard band
    s.set_mvp(camera(aspect));
    return s;
}

}  // namespace scenes
