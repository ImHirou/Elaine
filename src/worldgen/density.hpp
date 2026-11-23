// SPDX-License-Identifier: GPL-3.0-or-later
// The density functions of a world: vanilla's compiled samplers (net.minecraft.world.level.levelgen
// .densityfunction of the 26.3 jar - DensitySampler, SamplerContext, CachingDensitySampler and the
// sampler class of every function type), run from the generated program.
//
// The program (PROTO_GEN_WORLDGEN_HPP, kNodes) is what vanilla compiled, object for object: the
// extractor dumps the samplers RandomState.getSampler hands out, so references are inlined, caches
// shared, constants folded as vanilla folds them. This file is what each sampler class does, in
// vanilla's two ways:
//   value(node, x, y, z)       DensitySampler.sampleValue: one block
//   volume(node, out, volume)  DensitySampler.sampleVolume: a box of blocks at once
// They are different code in vanilla and do not always give the same bits - an interpolated
// function steps along y by adding in a volume and takes a three-way lerp at a point - and a
// cache (CachingDensitySampler) answers a point out of the last volume it was asked for when the
// point lies on that volume's grid. So which answer a caller gets depends on what was asked
// before, and a generator has to ask in vanilla's order. Everything here is kept as vanilla does
// it, lazy where vanilla is lazy: tests/unit/worldgen_test.cpp replays sequences of questions the
// jar's own samplers answered (Worldgen.Density*).
#pragma once

#include <memory>
#include <vector>

#include "worldgen/noise.hpp"

namespace worldgen {

// Buffers for the volumes a sampler works in, handed out and given back in the order of a stack
// (SamplerContext.acquireBuffer, closed when the sampler that asked is done): one after the
// other in blocks that are kept, so that nothing is allocated once a thread has made its first chunk.
class Scratch {
  public:
    struct Mark {
        u32 block, used;
    };
    float* take(u32 floats);
    Mark mark() const { return {block_, used_}; }
    void release(Mark m) {
        block_ = m.block;
        used_ = m.used;
    }

  private:
    // A whole chunk's volume is 98304 floats; a function is a dozen of them deep at most.
    static constexpr u32 kBlock = 1u << 20;
    std::vector<std::unique_ptr<float[]>> blocks_;
    u32 block_ = 0, used_ = 0;
};

// A SamplerContext: the caches of one piece of work (a NoiseChunk's, a climate sampler's) over a
// world's noises. Not shared between threads.
class Sampler {
  public:
    // `caches`: SamplerContext.Builder.enableCaches(). Without them a cached function is
    // computed every time it is asked (SamplerContext.EMPTY_UNCACHED).
    explicit Sampler(const Noises& noises, bool caches = true);
    ~Sampler();
    Sampler(const Sampler&) = delete;
    Sampler& operator=(const Sampler&) = delete;

    // A node of the program: proto::gen::worldgen::root::k*.
    float value(u32 node, i32 x, i32 y, i32 z);
    void volume(u32 node, float* out, const Volume& v);
    // A new context: every cache as empty as it was made.
    void reset();
    Scratch& scratch() { return scratch_; }

  private:
    // SamplerContext.CacheCell: the last volume a cached function was sampled over, and the last
    // single value.
    struct Cell {
        Volume volume{};
        float* buffer = nullptr;
        u32 capacity = 0;
        bool filled = false;
        i64 key = 0;
        float value;
    };
    struct PointInput;
    struct BufferInput;

    float cached_value(u32 cell, u32 input, i32 x, i32 y, i32 z);
    void cached_volume(u32 cell, u32 input, float* out, const Volume& v);
    void interpolate_blocks(const proto::gen::worldgen::Node& n, float* out, const Volume& v);
    i32 find_surface_from(const proto::gen::worldgen::Node& n, i32 x, i32 z, float upper_bound);
    template <class Input>
    float spline(u32 at, Input& in);
    const Noise& noise(u32 id) const;

    const Noises& noises_;
    bool caches_;
    Cell cells_[proto::gen::worldgen::kCacheCells];
    Scratch scratch_;
};

}  // namespace worldgen
