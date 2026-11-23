// SPDX-License-Identifier: GPL-3.0-or-later
// The noises a world is made of, ported from net.minecraft.world.level.levelgen.synth of the 26.3
// jar: GradientNoise, PerlinNoise, SmearedPerlinNoise, NoiseStack, and how NormalNoise and
// BlendedNoise (old_blended_noise) stack them.
//
// Vanilla samples a noise in two ways, and they do not give the same bits:
//   get(x, y, z)       one value: a layer is sampled at (x * frequency, ...), times its amplitude
//   add_to_volume(...) a whole volume of blocks added to a buffer: the scale and the frequency
//                      are multiplied first, the gradients of a column are looked up once per
//                      cell and their dot products are added up in another order
// Which one runs where is vanilla's choice (a chunk's density is a volume, an aquifer asks for
// values); both are here as they are there, and tests/unit/worldgen_test.cpp holds each to what
// the jar's classes answered (Worldgen.Noise*).
//
// A noise is a list of layers; what a seed draws of a layer - a permutation of 256 and three
// offsets - is drawn when a world's noises are made (Noises). What does not depend on the seed -
// which layers, their frequencies and amplitudes - is generated (PROTO_GEN_WORLDGEN_HPP).
#pragma once

#include <span>
#include <vector>

#include "base/types.hpp"
#include "proto/gen/current.hpp"
#include "worldgen/mth.hpp"
#include "worldgen/random.hpp"

#include PROTO_GEN_WORLDGEN_HPP

namespace worldgen {

// DensityVolume: a box of sample points, `step` blocks apart. A buffer over it is indexed
// y + (x + z * size_x) * size_y: a column of y is contiguous.
struct Volume {
    i32 size_x, size_y, size_z;
    i32 min_x, min_y, min_z;
    i32 step_x = 1, step_y = 1, step_z = 1;

    constexpr u32 size() const { return static_cast<u32>(size_x * size_y * size_z); }
    constexpr i32 block_x(i32 i) const { return min_x + i * step_x; }
    constexpr i32 block_y(i32 i) const { return min_y + i * step_y; }
    constexpr i32 block_z(i32 i) const { return min_z + i * step_z; }
    constexpr u32 index(i32 x, i32 y, i32 z) const { return static_cast<u32>(y + (x + z * size_x) * size_y); }
    bool operator==(const Volume&) const = default;
};

// GradientNoise with PerlinNoise's and SmearedPerlinNoise's sampling.
struct Perlin {
    double offset_x, offset_y, offset_z;
    double fudge_y_scale;  // SmearedPerlinNoise's; nothing reads it of a plain one
    u8 perms[256];

    // GradientNoise's constructor: three offsets, then the shuffle.
    template <class Random>
    void seed(Random& random) {
        offset_x = random.next_double() * 256.0;
        offset_y = random.next_double() * 256.0;
        offset_z = random.next_double() * 256.0;
        for (u32 i = 0; i < 256; ++i) perms[i] = static_cast<u8>(i);
        for (u32 i = 0; i < 256; ++i) {
            const u32 at = i + static_cast<u32>(random.next_int(static_cast<i32>(256 - i)));
            const u8 tmp = perms[i];
            perms[i] = perms[at];
            perms[at] = tmp;
        }
    }

    float get(double x, double y, double z) const;          // PerlinNoise.get
    float get_smeared(double x, double y, double z) const;  // SmearedPerlinNoise.get
    // PerlinNoise.addToVolume, SmearedPerlinNoise.addToVolume
    void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const;
    void add_to_volume_smeared(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const;
};

// NoiseStack.Layer
struct Layer {
    Perlin noise;
    double frequency;
    float amplitude;
};

// NoiseStack (NoiseStack.Perlin, NoiseStack.SmearedPerlin): a noise as the world uses it.
struct Noise {
    const Layer* layers = nullptr;
    u32 count = 0;
    bool smeared = false;

    float get(double x, double y, double z) const;
    float get(double x, double z) const;  // Noise.get(x, y): the layer's get(wrap(x), 0, wrap(y))
    void add_to_volume(float* buffer, const Volume& v, double xz_scale, double y_scale, float amplitude) const;
};

// Every noise of a world: RandomState.getOrCreateNoise for each of the registry, and the three
// stacks of old_blended_noise.
class Noises {
  public:
    explicit Noises(i64 seed);
    Noises(const Noises&) = delete;
    Noises& operator=(const Noises&) = delete;

    // By proto::gen::worldgen::noise::k*.
    const Noise& operator[](u32 id) const { return noises_[id]; }
    const Noise& blended_min_limit() const { return blended_[0]; }
    const Noise& blended_max_limit() const { return blended_[1]; }
    const Noise& blended_main() const { return blended_[2]; }
    // RandomState.random: what the seed of everything else in the world is forked from.
    const XoroshiroPositional& random() const { return random_; }

  private:
    XoroshiroPositional random_;
    std::vector<Layer> layers_;
    Noise noises_[proto::gen::worldgen::noise::kCount];
    Noise blended_[3];
};

}  // namespace worldgen
