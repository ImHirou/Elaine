// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/noise.hpp"

namespace worldgen {

namespace wg = proto::gen::worldgen;

namespace {

// GradientNoise.GRADIENT: sixteen directions, each with one component that is nothing.
constexpr float kGradient[16][3] = {{1, 1, 0},  {-1, 1, 0},  {1, -1, 0}, {-1, -1, 0}, {1, 0, 1},  {-1, 0, 1},
                                    {1, 0, -1}, {-1, 0, -1}, {0, 1, 1},  {0, -1, 1},  {0, 1, -1}, {0, -1, -1},
                                    {1, 1, 0},  {0, -1, 1},  {-1, 1, 0}, {0, -1, -1}};

// GradientNoise.wrap: a coordinate beyond 2^24 comes back into the noise's 2^25 (ROUND_OFF).
ALWAYS_INLINE double wrap(double x) {
    constexpr double kHalf = 0x1.fffffffffffffp+23;  // Math.nextDown(1.6777216E7)
    if (!(x < -kHalf) && !(x >= kHalf)) return x;
    return x - std::floor(x / 3.3554432E7 + 0.5) * 3.3554432E7;
}

// GradientNoise.permute
ALWAYS_INLINE i32 permute(const u8* perms, i32 x) { return perms[x & 255]; }

// GradientNoise.gradDot -> Gradient.dot(float, float, float): (gx * x + gy * y) + gz * z
ALWAYS_INLINE float grad_dot(i32 hash, float x, float y, float z) {
    const float* g = kGradient[hash & 15];
    return (g[0] * x + g[1] * y) + g[2] * z;
}

// PerlinNoise.sampleAndLerp: the eight corners of the cell, each its gradient's dot with the way
// to the point. `original_ry` is what the y weight is made of (a smeared noise moves `ry`).
ALWAYS_INLINE float sample_and_lerp(const u8* perms, i32 x, i32 y, i32 z, float rx, float ry, float rz, float original_ry) {
    const i32 x0 = permute(perms, x), x1 = permute(perms, x + 1);
    const i32 xy00 = permute(perms, x0 + y), xy01 = permute(perms, x0 + y + 1);
    const i32 xy10 = permute(perms, x1 + y), xy11 = permute(perms, x1 + y + 1);
    const float d000 = grad_dot(permute(perms, xy00 + z), rx, ry, rz);
    const float d100 = grad_dot(permute(perms, xy10 + z), rx - 1.0f, ry, rz);
    const float d010 = grad_dot(permute(perms, xy01 + z), rx, ry - 1.0f, rz);
    const float d110 = grad_dot(permute(perms, xy11 + z), rx - 1.0f, ry - 1.0f, rz);
    const float d001 = grad_dot(permute(perms, xy00 + z + 1), rx, ry, rz - 1.0f);
    const float d101 = grad_dot(permute(perms, xy10 + z + 1), rx - 1.0f, ry, rz - 1.0f);
    const float d011 = grad_dot(permute(perms, xy01 + z + 1), rx, ry - 1.0f, rz - 1.0f);
    const float d111 = grad_dot(permute(perms, xy11 + z + 1), rx - 1.0f, ry - 1.0f, rz - 1.0f);
    return lerp3(smoothstep(rx), smoothstep(original_ry), smoothstep(rz), d000, d100, d010, d110, d001, d101, d011, d111);
}

// SmearedPerlinNoise.computeFudgeY: how far down a point is pulled, so that a noise that is
// stretched along y comes in steps (the terrain's layers).
ALWAYS_INLINE double compute_fudge_y(double fudge_y_scale, double original_y, double relative_y) {
    const double limit = (!(original_y < 0.0) && !(original_y >= relative_y)) ? original_y : relative_y;
    return static_cast<double>(floor_int(limit / fudge_y_scale + 1.0000000116860974E-7)) * fudge_y_scale;
}

// The gradients of a cell as PerlinNoise.addToVolume keeps them along a column: the x and z part
// of each corner's dot product, and the corner's y component by itself.
struct Cell {
    float xz[8];  // d000xz, d100xz, d010xz, d110xz, d001xz, d101xz, d011xz, d111xz
    float gy[8];

    ALWAYS_INLINE void load(const u8* perms, i32 x0, i32 x1, i32 fy, i32 fz, float rx, float rz) {
        const i32 xy00 = permute(perms, x0 + fy), xy01 = permute(perms, x0 + fy + 1);
        const i32 xy10 = permute(perms, x1 + fy), xy11 = permute(perms, x1 + fy + 1);
        const auto corner = [&](u32 i, i32 hash, float x, float z) {
            const float* g = kGradient[permute(perms, hash) & 15];
            xz[i] = g[0] * x + g[2] * z;  // Gradient.dotXz
            gy[i] = g[1];
        };
        corner(0, xy00 + fz, rx, rz);
        corner(1, xy10 + fz, rx - 1.0f, rz);
        corner(2, xy01 + fz, rx, rz);
        corner(3, xy11 + fz, rx - 1.0f, rz);
        corner(4, xy00 + fz + 1, rx, rz - 1.0f);
        corner(5, xy10 + fz + 1, rx - 1.0f, rz - 1.0f);
        corner(6, xy01 + fz + 1, rx, rz - 1.0f);
        corner(7, xy11 + fz + 1, rx - 1.0f, rz - 1.0f);
    }
    // The value at `ry` up the cell (`ry - 1` down from its top), weighted by the three alphas.
    ALWAYS_INLINE float at(float alpha_x, float alpha_y, float alpha_z, float ry) const {
        return lerp3(alpha_x, alpha_y, alpha_z, xz[0] + gy[0] * ry, xz[1] + gy[1] * ry, xz[2] + gy[2] * (ry - 1.0f),
                     xz[3] + gy[3] * (ry - 1.0f), xz[4] + gy[4] * ry, xz[5] + gy[5] * ry, xz[6] + gy[6] * (ry - 1.0f),
                     xz[7] + gy[7] * (ry - 1.0f));
    }
};

}  // namespace

float Perlin::get(double _x, double _y, double _z) const {
    const double x = wrap(_x) + offset_x, y = wrap(_y) + offset_y, z = wrap(_z) + offset_z;
    const i32 fx = floor_int(x), fy = floor_int(y), fz = floor_int(z);
    const float rx = static_cast<float>(x - static_cast<double>(fx));
    const float ry = static_cast<float>(y - static_cast<double>(fy));
    const float rz = static_cast<float>(z - static_cast<double>(fz));
    return sample_and_lerp(perms, fx, fy, fz, rx, ry, rz, ry);
}

float Perlin::get_smeared(double _x, double _y, double _z) const {
    const double x = wrap(_x) + offset_x, y = wrap(_y) + offset_y, z = wrap(_z) + offset_z;
    const i32 fx = floor_int(x), fy = floor_int(y), fz = floor_int(z);
    const float rx = static_cast<float>(x - static_cast<double>(fx));
    const double ry = y - static_cast<double>(fy);
    const float rz = static_cast<float>(z - static_cast<double>(fz));
    const float fudged = static_cast<float>(ry - compute_fudge_y(fudge_y_scale, _y, ry));
    return sample_and_lerp(perms, fx, fy, fz, rx, fudged, rz, static_cast<float>(ry));
}

void Perlin::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    Cell cell;
    u32 index = 0;
    for (i32 iz = 0; iz < v.size_z; ++iz) {
        const double z = wrap(static_cast<double>(v.block_z(iz)) * xz_scale) + offset_z;
        const i32 fz = floor_int(z);
        const float rz = static_cast<float>(z - static_cast<double>(fz));
        const float alpha_z = smoothstep(rz);
        for (i32 ix = 0; ix < v.size_x; ++ix) {
            const double x = wrap(static_cast<double>(v.block_x(ix)) * xz_scale) + offset_x;
            const i32 fx = floor_int(x);
            const float rx = static_cast<float>(x - static_cast<double>(fx));
            const i32 x0 = permute(perms, fx), x1 = permute(perms, fx + 1);
            const float alpha_x = smoothstep(rx);
            i32 last_fy = INT32_MIN;
            for (i32 iy = 0; iy < v.size_y; ++iy, ++index) {
                const double y = wrap(static_cast<double>(v.block_y(iy)) * y_scale) + offset_y;
                const i32 fy = floor_int(y);
                const float ry = static_cast<float>(y - static_cast<double>(fy));
                const float alpha_y = smoothstep(ry);
                if (last_fy != fy) {
                    cell.load(perms, x0, x1, fy, fz, rx, rz);
                    last_fy = fy;
                }
                buffer[index] = buffer[index] + amplitude * cell.at(alpha_x, alpha_y, alpha_z, ry);
            }
        }
    }
}

void Perlin::add_to_volume_smeared(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    Cell cell;
    u32 index = 0;
    for (i32 iz = 0; iz < v.size_z; ++iz) {
        const double z = wrap(static_cast<double>(v.block_z(iz)) * xz_scale) + offset_z;
        const i32 fz = floor_int(z);
        const float rz = static_cast<float>(z - static_cast<double>(fz));
        const float alpha_z = smoothstep(rz);
        for (i32 ix = 0; ix < v.size_x; ++ix) {
            const double x = wrap(static_cast<double>(v.block_x(ix)) * xz_scale) + offset_x;
            const i32 fx = floor_int(x);
            const float rx = static_cast<float>(x - static_cast<double>(fx));
            const i32 x0 = permute(perms, fx), x1 = permute(perms, fx + 1);
            const float alpha_x = smoothstep(rx);
            i32 last_fy = INT32_MIN;
            for (i32 iy = 0; iy < v.size_y; ++iy, ++index) {
                const double original_y = static_cast<double>(v.block_y(iy)) * y_scale;
                const double y = wrap(original_y) + offset_y;
                const i32 fy = floor_int(y);
                const double ry = y - static_cast<double>(fy);
                const float alpha_y = smoothstep(static_cast<float>(ry));
                if (last_fy != fy) {
                    cell.load(perms, x0, x1, fy, fz, rx, rz);
                    last_fy = fy;
                }
                const float fudged = static_cast<float>(ry - compute_fudge_y(fudge_y_scale, original_y, ry));
                buffer[index] = buffer[index] + amplitude * cell.at(alpha_x, alpha_y, alpha_z, fudged);
            }
        }
    }
}

float Simplex::get(double xin, double yin) const {
    // (No offsets: these noises are made with them thrown away.)
    static const double kSqrt3 = std::sqrt(3.0);
    static const double kF2 = 0.5 * (kSqrt3 - 1.0), kG2 = (3.0 - kSqrt3) / 6.0;
    const double s = (xin + yin) * kF2;
    const i32 i = floor_int(xin + s), j = floor_int(yin + s);
    const double t = static_cast<double>(i + j) * kG2;
    const double x0 = xin - (static_cast<double>(i) - t), y0 = yin - (static_cast<double>(j) - t);
    const i32 i1 = x0 > y0 ? 1 : 0, j1 = x0 > y0 ? 0 : 1;
    const double x1 = x0 - static_cast<double>(i1) + kG2, y1 = y0 - static_cast<double>(j1) + kG2;
    const double x2 = x0 - 1.0 + 2.0 * kG2, y2 = y0 - 1.0 + 2.0 * kG2;
    const i32 ii = i & 255, jj = j & 255;
    // getCornerNoise3D(index, x, y, 0, 0.5): a corner's share, which falls off to nothing at 0.5.
    const auto corner = [](i32 index, double x, double y) {
        const double z = 0.0;
        double share = 0.5 - x * x - y * y - z * z;
        if (share < 0.0) return 0.0;
        share = share * share;
        const float* g = kGradient[index];
        return share * share * (static_cast<double>(g[0]) * x + static_cast<double>(g[1]) * y + static_cast<double>(g[2]) * z);
    };
    const double n0 = corner(permute(perms, ii + permute(perms, jj)) % 12, x0, y0);
    const double n1 = corner(permute(perms, ii + i1 + permute(perms, jj + j1)) % 12, x1, y1);
    const double n2 = corner(permute(perms, ii + 1 + permute(perms, jj + 1)) % 12, x2, y2);
    return static_cast<float>(70.0 * (n0 + n1 + n2));
}

float Noise::get(double x, double y, double z) const {
    float value = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const Layer& l = layers[i];
        const double f = l.frequency;
        value = value + l.amplitude * (smeared ? l.noise.get_smeared(x * f, y * f, z * f) : l.noise.get(x * f, y * f, z * f));
    }
    return value;
}

float Noise::get(double x, double z) const {
    float value = 0.0f;
    for (u32 i = 0; i < count; ++i) {
        const Layer& l = layers[i];
        const double f = l.frequency;
        // PerlinNoise.get(x, y): this.get(wrap(x), 0, wrap(y)) - which wraps once more, and changes nothing.
        const double wx = wrap(x * f), wz = wrap(z * f);
        value = value + l.amplitude * (smeared ? l.noise.get_smeared(wx, 0.0, wz) : l.noise.get(wx, 0.0, wz));
    }
    return value;
}

void Noise::add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const {
    for (u32 i = 0; i < count; ++i) {
        const Layer& l = layers[i];
        const double f = l.frequency;
        if (smeared)
            l.noise.add_to_volume_smeared(buffer, v, xz_scale * f, y_scale * f, amplitude * l.amplitude);
        else
            l.noise.add_to_volume(buffer, v, xz_scale * f, y_scale * f, amplitude * l.amplitude);
    }
}

Noises::Noises(i64 seed) : random_(XoroshiroRandom(seed).fork_positional()) {
    constexpr u32 kBlended = wg::kBlendedMinLimit + wg::kBlendedMaxLimit + wg::kBlendedMain;
    layers_.resize(std::size(wg::kNoiseLayers) + kBlended);
    // Noises.instantiate -> NormalNoise.create: a source by the noise's name, two factories forked
    // from it, and each octave's two generators from the octave's name - one of either factory.
    for (u32 id = 0; id < wg::noise::kCount; ++id) {
        const wg::Noise& n = wg::kNoises[id];
        XoroshiroRandom source = random_.from_hash({n.hash.lo, n.hash.hi});
        const XoroshiroPositional first = source.fork_positional(), second = source.fork_positional();
        for (u32 i = 0; i < n.layers; ++i) {
            const wg::NoiseLayer& made = wg::kNoiseLayers[n.first_layer + i];
            Layer& l = layers_[n.first_layer + i];
            const wg::Hash& octave = wg::kOctaveHash[made.octave - wg::kFirstOctave];
            XoroshiroRandom r = (made.second ? second : first).from_hash({octave.lo, octave.hi});
            l.noise.seed(r);
            l.noise.fudge_y_scale = 0.0;
            l.frequency = made.frequency;
            l.amplitude = made.amplitude;
        }
        noises_[id] = {&layers_[n.first_layer], n.layers, false};
    }
    // BlendedNoise.createFbmSet: one source, and every layer of the three stacks takes its turn at it.
    XoroshiroRandom terrain = random_.from_hash({wg::kBlendedHash.lo, wg::kBlendedHash.hi});
    const u32 base = static_cast<u32>(std::size(wg::kNoiseLayers));
    for (u32 i = 0; i < kBlended; ++i) {
        Layer& l = layers_[base + i];
        l.noise.seed(terrain);
        l.noise.fudge_y_scale = wg::kBlendedLayers[i].fudge_y_scale;
        l.frequency = wg::kBlendedLayers[i].frequency;
        l.amplitude = wg::kBlendedLayers[i].amplitude;
    }
    blended_[0] = {&layers_[base], wg::kBlendedMinLimit, true};
    blended_[1] = {&layers_[base + wg::kBlendedMinLimit], wg::kBlendedMaxLimit, true};
    blended_[2] = {&layers_[base + wg::kBlendedMinLimit + wg::kBlendedMaxLimit], wg::kBlendedMain, true};
}

}  // namespace worldgen
