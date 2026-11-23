// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/density.hpp"

#include <cstdlib>
#include <limits>

namespace worldgen {

namespace wg = proto::gen::worldgen;
using wg::Node;
using wg::Op;

namespace {

constexpr u32 kMaxCoordinates = 8;  // of one spline function (the overworld's have four at most)
constexpr u32 kMaxSelect = 16;      // ...and the samplers of one interval select (five)

// BlockPos.asLong: 26 bits of x, 26 of z, 12 of y - a cache's key for one value.
ALWAYS_INLINE i64 block_key(i32 x, i32 y, i32 z) {
    return static_cast<i64>((static_cast<u64>(static_cast<u32>(x) & 0x3FFFFFFu) << 38) |
                            (static_cast<u64>(static_cast<u32>(z) & 0x3FFFFFFu) << 12) | (static_cast<u32>(y) & 0xFFFu));
}

// GradientFunction.ClampedSampler.compute
ALWAYS_INLINE float gradient(const Node& n, i32 coordinate) {
    const i32 relative = std::min(std::max(coordinate, n.i1), n.i2) - n.i0;
    return n.f0 + static_cast<float>(relative) * n.f1;
}

ALWAYS_INLINE float cube(float x) { return (x * x) * x; }  // Mth.cube
// UnaryFunction.SqueezeSampler.apply
ALWAYS_INLINE float squeeze(float x) {
    const float c = clamp(x, -1.0f, 1.0f);
    return c / 2.0f - cube(c) / 24.0f;
}
// UnaryFunction.LeakyReLUSampler.apply (half_negative, quarter_negative)
ALWAYS_INLINE float leaky(float x, float factor) { return x > 0.0f ? x : x * factor; }

// IntervalSelectFunction.Sampler.selectSamplerIndex: the first threshold the input is below.
ALWAYS_INLINE u32 select_index(const Node& n, float input) {
    for (u32 i = 0; i + 1 < n.count; ++i)
        if (input < wg::kSelectThresholds[n.list + i]) return i;
    return n.count - 1u;
}

// CubicSpline.Multipoint.linearExtend
ALWAYS_INLINE float linear_extend(float input, const wg::SplinePoint& p, float value) {
    return p.derivative == 0.0f ? value : value + p.derivative * (input - p.location);
}

}  // namespace

float* Scratch::take(u32 floats) {
    DCHECK(floats <= kBlock);
    if (blocks_.empty() || used_ + floats > kBlock) {
        if (!blocks_.empty()) ++block_;
        if (block_ == blocks_.size()) blocks_.emplace_back(new float[kBlock]);
        used_ = 0;
    }
    float* p = blocks_[block_].get() + used_;
    used_ += floats;
    return p;
}

Sampler::Sampler(const Noises& noises, bool caches) : noises_(noises), caches_(caches) { reset(); }

Sampler::~Sampler() {
    for (Cell& c : cells_) std::free(c.buffer);
}

void Sampler::reset() {
    for (Cell& c : cells_) {
        c.filled = false;
        c.key = 0;
        c.value = std::numeric_limits<float>::quiet_NaN();
    }
}

const Noise& Sampler::noise(u32 id) const {
    if (id < wg::noise::kCount) return noises_[id];
    return id == wg::noise::kCount       ? noises_.blended_min_limit()
           : id == wg::noise::kCount + 1 ? noises_.blended_max_limit()
                                         : noises_.blended_main();
}

// SamplerContext.sampleValueCached: the value it gave for this very block last, else what the
// volume it holds says of the block if the block is one of its points, else the function itself.
float Sampler::cached_value(u32 cell, u32 input, i32 x, i32 y, i32 z) {
    if (!caches_) return value(input, x, y, z);
    Cell& c = cells_[cell];
    const i64 key = block_key(x, y, z);
    if (c.key == key && !std::isnan(c.value)) return c.value;
    if (c.filled) {
        const i32 index = c.volume.index_of_block(x, y, z);
        if (index != -1) return c.buffer[index];
    }
    const float v = value(input, x, y, z);
    c.key = key;
    c.value = v;
    return v;
}

// SamplerContext.sampleVolumeCached: the volume it holds if that is the one asked for.
void Sampler::cached_volume(u32 cell, u32 input, float* out, const Volume& v) {
    if (!caches_) return volume(input, out, v);
    Cell& c = cells_[cell];
    const u32 size = v.size();
    if (!c.filled || !(v == c.volume)) {
        c.volume = v;
        if (c.capacity < size) {
            std::free(c.buffer);
            c.buffer = static_cast<float*>(std::malloc(size * sizeof(float)));
            c.capacity = size;
        }
        c.filled = true;
        volume(input, c.buffer, v);
    }
    std::memcpy(out, c.buffer, size * sizeof(float));
}

// SplineFunction.PointSplineInput: a coordinate is sampled when a spline first asks for it.
struct Sampler::PointInput {
    Sampler& s;
    const u16* samplers;
    i32 x, y, z;
    float cached[kMaxCoordinates];

    float coordinate(u32 index) {
        if (!std::isnan(cached[index])) return cached[index];
        return cached[index] = s.value(samplers[index], x, y, z);
    }
};

// SplineFunction.BufferSplineInput: ...over the whole volume, when the first point asks.
struct Sampler::BufferInput {
    Sampler& s;
    const u16* samplers;
    const Volume& v;
    float* buffers[kMaxCoordinates];
    u32 index;

    float coordinate(u32 at) {
        if (!buffers[at]) {
            buffers[at] = s.scratch_.take(v.size());
            s.volume(samplers[at], buffers[at], v);
        }
        return buffers[at][index];
    }
};

// CubicSpline.Multipoint.sample
template <class Input>
float Sampler::spline(u32 at, Input& in) {
    const wg::Spline& sp = wg::kSplines[at];
    const wg::SplinePoint* points = &wg::kSplinePoints[sp.first_point];
    const auto height = [&](const wg::SplinePoint& p) { return p.spline < 0 ? p.value : spline(static_cast<u32>(p.spline), in); };
    const float input = in.coordinate(sp.coordinate);
    // findIntervalStart: Mth.binarySearch for the first location beyond the input, less one.
    i32 from = 0;
    for (i32 len = sp.points; len > 0;) {
        const i32 half = len / 2, middle = from + half;
        if (input < points[middle].location) {
            len = half;
        } else {
            from = middle + 1;
            len -= half + 1;
        }
    }
    const i32 start = from - 1, last = sp.points - 1;
    if (start < 0) return linear_extend(input, points[0], height(points[0]));
    if (start == last) return linear_extend(input, points[last], height(points[last]));
    const wg::SplinePoint& p1 = points[start];
    const wg::SplinePoint& p2 = points[start + 1];
    const float x1 = p1.location, x2 = p2.location;
    const float t = (input - x1) / (x2 - x1);
    const float d1 = p1.derivative, d2 = p2.derivative;
    const float y1 = height(p1);
    const float y2 = height(p2);
    const float a = d1 * (x2 - x1) - (y2 - y1);
    const float b = -d2 * (x2 - x1) + (y2 - y1);
    return lerp(t, y1, y2) + (t * (1.0f - t)) * lerp(t, a, b);
}

// FindTopSurfaceFunction.Sampler.findSurfaceFrom: down from the upper bound in steps of a cell,
// the first place the density is above nothing.
i32 Sampler::find_surface_from(const Node& n, i32 x, i32 z, float upper_bound) {
    const i32 lower = n.i0, cell = n.i1;
    const i32 top = floor_int(upper_bound / static_cast<float>(cell)) * cell;
    if (top <= lower) return lower;
    for (i32 y = top; y >= lower; y -= cell)
        if (value(n.a, x, y, z) > 0.0f) return y;
    return lower;
}

float Sampler::value(u32 id, i32 x, i32 y, i32 z) {
    const Node& n = wg::kNodes[id];
    switch (n.op) {
        case Op::kConstant: return n.f0;
        case Op::kAdd: {
            const float l = value(n.a, x, y, z);
            return l + value(n.b, x, y, z);
        }
        case Op::kSub: {
            const float l = value(n.a, x, y, z);
            return l - value(n.b, x, y, z);
        }
        case Op::kMul: {  // (a product with nothing is nothing, and its other half is not asked)
            const float l = value(n.a, x, y, z);
            if (l == 0.0f) return 0.0f;
            return l * value(n.b, x, y, z);
        }
        case Op::kDiv: {
            const float l = value(n.a, x, y, z);
            if (l == 0.0f) return 0.0f;
            return l / value(n.b, x, y, z);
        }
        case Op::kMin: {  // `f0`: the least the right side can be - at or below it the left side is the answer
            const float l = value(n.a, x, y, z);
            if (l <= n.f0) return l;
            return java_min(l, value(n.b, x, y, z));
        }
        case Op::kMax: {
            const float l = value(n.a, x, y, z);
            if (l >= n.f0) return l;
            return java_max(l, value(n.b, x, y, z));
        }
        case Op::kConstAdd: return value(n.a, x, y, z) + n.f0;
        case Op::kConstMul: return value(n.a, x, y, z) * n.f0;
        case Op::kConstMin: return java_min(value(n.a, x, y, z), n.f0);
        case Op::kConstMax: return java_max(value(n.a, x, y, z), n.f0);
        case Op::kConstSub: return n.f0 - value(n.a, x, y, z);
        case Op::kConstDiv: return n.f0 / value(n.a, x, y, z);
        case Op::kAbs: return std::fabs(value(n.a, x, y, z));
        case Op::kCube: return cube(value(n.a, x, y, z));
        case Op::kNegate: return -value(n.a, x, y, z);
        case Op::kSquare: {
            const float v = value(n.a, x, y, z);
            return v * v;
        }
        case Op::kSqueeze: return squeeze(value(n.a, x, y, z));
        case Op::kLeakyRelu: return leaky(value(n.a, x, y, z), n.f0);
        case Op::kClamp: return clamp(value(n.a, x, y, z), n.f0, n.f1);
        case Op::kRangeChoice: {
            const float in = value(n.a, x, y, z);
            return (in >= n.f0 && in < n.f1) ? value(n.b, x, y, z) : value(n.c, x, y, z);
        }
        case Op::kRangeChoiceConst: {
            const float in = value(n.a, x, y, z);
            return (in >= n.f0 && in < n.f1) ? n.f2 : n.f3;
        }
        case Op::kIntervalSelect: {
            const float in = value(n.a, x, y, z);
            return value(wg::kSelect[n.list + select_index(n, in)], x, y, z);
        }
        case Op::kLerp: {
            const float alpha = value(n.a, x, y, z);
            if (alpha == 0.0f) return value(n.b, x, y, z);
            if (alpha == 1.0f) return value(n.c, x, y, z);
            const float first = value(n.b, x, y, z);
            return lerp(alpha, first, value(n.c, x, y, z));
        }
        case Op::kLerpConstFirst: {
            const float alpha = value(n.a, x, y, z);
            if (alpha == 0.0f) return n.f0;
            if (alpha == 1.0f) return value(n.b, x, y, z);
            return lerp(alpha, n.f0, value(n.b, x, y, z));
        }
        case Op::kSliceY: return value(n.a, x, n.i0, z);
        case Op::kSliceXz: return value(n.a, n.i0, y, n.i1);
        case Op::kGradient: return gradient(n, n.axis == 0 ? x : n.axis == 1 ? y : z);
        case Op::kNoise:
            return noise(n.noise).get(static_cast<double>(x) * n.d0, static_cast<double>(y) * n.d1,
                                      static_cast<double>(z) * n.d0);
        case Op::kShiftedXzNoise: {
            const double nx = static_cast<double>(x) * n.d0 + static_cast<double>(value(n.a, x, y, z));
            const double ny = static_cast<double>(y) * n.d1;
            const double nz = static_cast<double>(z) * n.d0 + static_cast<double>(value(n.b, x, y, z));
            return noise(n.noise).get(nx, ny, nz);
        }
        case Op::kShiftB: return noise(n.noise).get(static_cast<double>(z) * 0.25, static_cast<double>(x) * 0.25, 0.0) * 4.0f;
        case Op::kInterpolated: {
            // On a corner of a cell it is its input; elsewhere the eight corners around, mixed
            // three ways (which is not how a volume mixes them).
            const i32 xz = n.i0, ys = n.i1;
            const i32 cx = floor_mod(x, xz), cy = floor_mod(y, ys), cz = floor_mod(z, xz);
            if (cx == 0 && cy == 0 && cz == 0) return value(n.a, x, y, z);
            const Volume v{2, 2, 2, x - cx, y - cy, z - cz, xz, ys, xz};
            const Scratch::Mark mark = scratch_.mark();
            float* b = scratch_.take(8);
            volume(n.a, b, v);
            const float out = lerp3(
                static_cast<float>(cx) / static_cast<float>(xz), static_cast<float>(cy) / static_cast<float>(ys),
                static_cast<float>(cz) / static_cast<float>(xz), b[v.index(0, 0, 0)], b[v.index(1, 0, 0)], b[v.index(0, 1, 0)],
                b[v.index(1, 1, 0)], b[v.index(0, 0, 1)], b[v.index(1, 0, 1)], b[v.index(0, 1, 1)], b[v.index(1, 1, 1)]);
            scratch_.release(mark);
            return out;
        }
        case Op::kSpline: {
            DCHECK(n.count <= kMaxCoordinates);
            PointInput in{*this, &wg::kSplineCoordinates[n.i0], x, y, z, {}};
            for (float& c : in.cached) c = std::numeric_limits<float>::quiet_NaN();
            return spline(n.list, in);
        }
        case Op::kFindTopSurface: return static_cast<float>(find_surface_from(n, x, z, value(n.b, x, y, z)));
        case Op::kBlendDensity: return value(n.a, x, y, z);  // (no chunk of an older world to blend with)
        case Op::kContextBound: return value(n.a, x, y, z);  // (nothing bound: the sampler it falls back to)
        case Op::kCache: return cached_value(static_cast<u32>(n.i0), n.a, x, y, z);
    }
    return 0.0f;
}

// InterpolatedFunction.Sampler.sampleWithBlockStep: the input at the corners of the cells the
// volume lies in, and every block between them - along z, along x, then up a column in equal
// steps, each the one before plus the step.
void Sampler::interpolate_blocks(const Node& n, float* out, const Volume& v) {
    const i32 xz = n.i0, ys = n.i1;
    const float xz_inv = n.f0, y_inv = n.f1;
    const i32 max_x = v.min_x + v.size_x * v.step_x - 1, max_y = v.min_y + v.size_y * v.step_y - 1,
              max_z = v.min_z + v.size_z * v.step_z - 1;
    const i32 min_cx = floor_div(v.min_x, xz), min_cy = floor_div(v.min_y, ys), min_cz = floor_div(v.min_z, xz);
    const i32 count_x = floor_div(max_x, xz) - min_cx + 1, count_y = floor_div(max_y, ys) - min_cy + 1;
    const i32 count_z = floor_div(max_z, xz) - min_cz + 1;
    // One more row of corners behind the last cell, unless the volume ends on a corner.
    const Volume cells{floor_mod(max_x, xz) == 0 ? count_x : count_x + 1,
                       floor_mod(max_y, ys) == 0 ? count_y : count_y + 1,
                       floor_mod(max_z, xz) == 0 ? count_z : count_z + 1,
                       min_cx * xz,
                       min_cy * ys,
                       min_cz * xz,
                       xz,
                       ys,
                       xz};
    const Scratch::Mark mark = scratch_.mark();
    float* corner = scratch_.take(cells.size());
    volume(n.a, corner, cells);
    for (i32 cz = 0; cz < count_z; ++cz) {
        const i32 nz = std::min(cz + 1, cells.size_z - 1);
        for (i32 cx = 0; cx < count_x; ++cx) {
            const i32 nx = std::min(cx + 1, cells.size_x - 1);
            float v000 = corner[cells.index(cx, 0, cz)], v100 = corner[cells.index(nx, 0, cz)];
            float v001 = corner[cells.index(cx, 0, nz)], v101 = corner[cells.index(nx, 0, nz)];
            for (i32 cy = 0; cy < count_y; ++cy) {
                const i32 ny = std::min(cy + 1, cells.size_y - 1);
                const float v010 = corner[cells.index(cx, ny, cz)], v110 = corner[cells.index(nx, ny, cz)];
                const float v011 = corner[cells.index(cx, ny, nz)], v111 = corner[cells.index(nx, ny, nz)];
                // fillCell: the part of this cell that lies in the volume.
                const i32 ox = cells.block_x(cx) - v.min_x, oy = cells.block_y(cy) - v.min_y, oz = cells.block_z(cz) - v.min_z;
                const i32 x0 = std::max(0, -ox), y0 = std::max(0, -oy), z0 = std::max(0, -oz);
                const i32 x1 = std::min(xz, v.size_x - ox) - 1, y1 = std::min(ys, v.size_y - oy) - 1,
                          z1 = std::min(xz, v.size_z - oz) - 1;
                for (i32 z = z0; z <= z1; ++z) {
                    const float alpha_z = static_cast<float>(z) * xz_inv;
                    const float v00 = lerp(alpha_z, v000, v001), v01 = lerp(alpha_z, v010, v011);
                    const float v10 = lerp(alpha_z, v100, v101), v11 = lerp(alpha_z, v110, v111);
                    for (i32 x = x0; x <= x1; ++x) {
                        const float alpha_x = static_cast<float>(x) * xz_inv;
                        const float low = lerp(alpha_x, v00, v10), high = lerp(alpha_x, v01, v11);
                        const float step = (high - low) * y_inv;
                        float value = low + step * static_cast<float>(y0);
                        u32 index = v.index(ox + x, oy + y0, oz + z);
                        for (i32 y = y0; y <= y1; ++y) {
                            out[index++] = value;
                            value = value + step;
                        }
                    }
                }
                v000 = v010;
                v100 = v110;
                v001 = v011;
                v101 = v111;
            }
        }
    }
    scratch_.release(mark);
}

void Sampler::volume(u32 id, float* out, const Volume& v) {
    const Node& n = wg::kNodes[id];
    const u32 size = v.size();
    const Scratch::Mark mark = scratch_.mark();
    switch (n.op) {
        case Op::kConstant: std::fill_n(out, size, n.f0); break;
        case Op::kAdd: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] + r[i];
            break;
        }
        case Op::kSub: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] + -r[i];
            break;
        }
        case Op::kMul: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] * r[i];
            break;
        }
        case Op::kDiv: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] / r[i];
            break;
        }
        case Op::kMin: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i)
                if (r[i] < out[i]) out[i] = r[i];
            break;
        }
        case Op::kMax: {
            volume(n.a, out, v);
            float* r = scratch_.take(size);
            volume(n.b, r, v);
            for (u32 i = 0; i < size; ++i)
                if (r[i] > out[i]) out[i] = r[i];
            break;
        }
        case Op::kConstAdd:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] + n.f0;
            break;
        case Op::kConstMul:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] * n.f0;
            break;
        case Op::kConstMin:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i)
                if (n.f0 < out[i]) out[i] = n.f0;
            break;
        case Op::kConstMax:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i)
                if (n.f0 > out[i]) out[i] = n.f0;
            break;
        case Op::kConstSub:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = n.f0 - out[i];
            break;
        case Op::kConstDiv:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = n.f0 / out[i];
            break;
        case Op::kAbs:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = std::fabs(out[i]);
            break;
        case Op::kCube:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = cube(out[i]);
            break;
        case Op::kNegate:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = -out[i];
            break;
        case Op::kSquare:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = out[i] * out[i];
            break;
        case Op::kSqueeze:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = squeeze(out[i]);
            break;
        case Op::kLeakyRelu:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = leaky(out[i], n.f0);
            break;
        case Op::kClamp:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = clamp(out[i], n.f0, n.f1);
            break;
        case Op::kRangeChoice: {  // (all three over the whole volume, in this order)
            volume(n.b, out, v);
            float* in = scratch_.take(size);
            volume(n.a, in, v);
            float* other = scratch_.take(size);
            volume(n.c, other, v);
            for (u32 i = 0; i < size; ++i)
                if (!(in[i] >= n.f0 && in[i] < n.f1)) out[i] = other[i];
            break;
        }
        case Op::kRangeChoiceConst:
            volume(n.a, out, v);
            for (u32 i = 0; i < size; ++i) out[i] = (out[i] >= n.f0 && out[i] < n.f1) ? n.f2 : n.f3;
            break;
        case Op::kIntervalSelect: {
            volume(n.a, out, v);
            float* buffers[kMaxSelect];
            DCHECK(n.count <= kMaxSelect);
            for (u32 k = 0; k < n.count; ++k) {
                buffers[k] = scratch_.take(size);
                volume(wg::kSelect[n.list + k], buffers[k], v);
            }
            for (u32 i = 0; i < size; ++i) out[i] = buffers[select_index(n, out[i])][i];
            break;
        }
        case Op::kLerp: {
            volume(n.a, out, v);
            float* first = scratch_.take(size);
            volume(n.b, first, v);
            float* second = scratch_.take(size);
            volume(n.c, second, v);
            for (u32 i = 0; i < size; ++i) {
                const float alpha = out[i];
                out[i] = alpha == 0.0f ? first[i] : alpha == 1.0f ? second[i] : lerp(alpha, first[i], second[i]);
            }
            break;
        }
        case Op::kLerpConstFirst: {
            volume(n.a, out, v);
            float* second = scratch_.take(size);
            volume(n.b, second, v);
            for (u32 i = 0; i < size; ++i) {
                const float alpha = out[i];
                out[i] = alpha == 0.0f ? n.f0 : alpha == 1.0f ? second[i] : lerp(alpha, n.f0, second[i]);
            }
            break;
        }
        case Op::kSliceY: {
            // A function that does not depend on y: one layer of it, at `i0`, for every y.
            if (v.size_y == 1 && v.min_y == n.i0) {
                volume(n.a, out, v);
                break;
            }
            const Volume layer{v.size_x, 1, v.size_z, v.min_x, n.i0, v.min_z, v.step_x, v.step_y, v.step_z};
            float* in = scratch_.take(layer.size());
            volume(n.a, in, layer);
            for (i32 z = 0; z < v.size_z; ++z)
                for (i32 x = 0; x < v.size_x; ++x) std::fill_n(out + v.index(x, 0, z), v.size_y, in[layer.index(x, 0, z)]);
            break;
        }
        case Op::kSliceXz: {
            if (v.size_x == 1 && v.size_z == 1 && v.min_x == n.i0 && v.min_z == n.i1) {
                volume(n.a, out, v);
                break;
            }
            const Volume column{1, v.size_y, 1, n.i0, v.min_y, n.i1, v.step_x, v.step_y, v.step_z};
            float* in = scratch_.take(column.size());
            volume(n.a, in, column);
            for (i32 y = 0; y < v.size_y; ++y) {
                const float value = in[column.index(0, y, 0)];
                u32 index = v.index(0, y, 0);
                for (i32 k = 0; k < v.size_z * v.size_x; ++k, index += static_cast<u32>(v.size_y)) out[index] = value;
            }
            break;
        }
        case Op::kGradient:
            if (n.axis == 0) {
                for (i32 x = 0; x < v.size_x; ++x) {
                    const float value = gradient(n, v.block_x(x));
                    for (i32 z = 0; z < v.size_z; ++z) std::fill_n(out + v.index(x, 0, z), v.size_y, value);
                }
            } else if (n.axis == 1) {
                for (i32 y = 0; y < v.size_y; ++y) {
                    const float value = gradient(n, v.block_y(y));
                    for (i32 z = 0; z < v.size_z; ++z)
                        for (i32 x = 0; x < v.size_x; ++x) out[v.index(x, y, z)] = value;
                }
            } else {
                for (i32 z = 0; z < v.size_z; ++z)
                    std::fill_n(out + v.index(0, 0, z), v.size_x * v.size_y, gradient(n, v.block_z(z)));
            }
            break;
        case Op::kNoise:
            std::fill_n(out, size, 0.0f);
            noise(n.noise).add_to_volume(out, v, n.d0, n.d1, 1.0f);
            break;
        case Op::kShiftedXzNoise: {
            // A noise sampled where two others say: one value at a time, at the points they shift to.
            volume(n.a, out, v);
            float* shift_z = scratch_.take(size);
            volume(n.b, shift_z, v);
            const Noise& noise_ = noise(n.noise);
            u32 index = 0;
            for (i32 z = 0; z < v.size_z; ++z) {
                const double base_z = static_cast<double>(v.block_z(z)) * n.d0;
                for (i32 x = 0; x < v.size_x; ++x) {
                    const double base_x = static_cast<double>(v.block_x(x)) * n.d0;
                    for (i32 y = 0; y < v.size_y; ++y, ++index) {
                        const double nx = base_x + static_cast<double>(out[index]);
                        const double ny = static_cast<double>(v.block_y(y)) * n.d1;
                        const double nz = base_z + static_cast<double>(shift_z[index]);
                        out[index] = noise_.get(nx, ny, nz);
                    }
                }
            }
            break;
        }
        case Op::kShiftB: {
            // shift_b is its noise at (z, x, 0): a layer of it, with x and z changed about.
            const Volume turned{v.size_z, v.size_x, 1, v.min_z, v.min_x, 0, v.step_z, v.step_x, 1};
            float* in = scratch_.take(turned.size());
            std::fill_n(in, turned.size(), 0.0f);
            noise(n.noise).add_to_volume(in, turned, 0.25, 0.25, 4.0f);
            for (i32 z = 0; z < v.size_z; ++z)
                for (i32 x = 0; x < v.size_x; ++x) std::fill_n(out + v.index(x, 0, z), v.size_y, in[turned.index(z, x, 0)]);
            break;
        }
        case Op::kInterpolated: {
            const i32 xz = n.i0, ys = n.i1;
            // A volume of nothing but corners of cells is the input itself.
            if ((v.step_x == xz || v.size_x == 1) && (v.step_y == ys || v.size_y == 1) && (v.step_z == xz || v.size_z == 1) &&
                floor_mod(v.min_x, xz) == 0 && floor_mod(v.min_y, ys) == 0 && floor_mod(v.min_z, xz) == 0) {
                volume(n.a, out, v);
                break;
            }
            if (v.step_x == 1 && v.step_y == 1 && v.step_z == 1) {
                interpolate_blocks(n, out, v);
                break;
            }
            // sampleWithNonBlockStep: every block of the box, and of those the ones that were asked for.
            const Volume blocks{
                v.size_x * v.step_x, v.size_y * v.step_y, v.size_z * v.step_z, v.min_x, v.min_y, v.min_z, 1, 1, 1};
            float* all = scratch_.take(blocks.size());
            interpolate_blocks(n, all, blocks);
            for (i32 z = 0; z < v.size_z; ++z)
                for (i32 x = 0; x < v.size_x; ++x)
                    for (i32 y = 0; y < v.size_y; ++y)
                        out[v.index(x, y, z)] = all[blocks.index(x * v.step_x, y * v.step_y, z * v.step_z)];
            break;
        }
        case Op::kSpline: {
            DCHECK(n.count <= kMaxCoordinates);
            BufferInput in{*this, &wg::kSplineCoordinates[n.i0], v, {}, 0};
            for (u32 i = 0; i < size; ++i) {
                in.index = i;
                out[i] = spline(n.list, in);
            }
            break;
        }
        case Op::kFindTopSurface: {
            DCHECK(v.size_y == 1);
            volume(n.b, out, v);
            u32 index = 0;
            for (i32 z = 0; z < v.size_z; ++z)
                for (i32 x = 0; x < v.size_x; ++x, ++index)
                    out[index] = static_cast<float>(find_surface_from(n, v.block_x(x), v.block_z(z), out[index]));
            break;
        }
        case Op::kBlendDensity: volume(n.a, out, v); break;
        case Op::kContextBound: volume(n.a, out, v); break;
        case Op::kCache: cached_volume(static_cast<u32>(n.i0), n.a, out, v); break;
    }
    scratch_.release(mark);
}

}  // namespace worldgen
