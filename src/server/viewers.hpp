// SPDX-License-Identifier: GPL-3.0-or-later
// Which players have a chunk in their view: the scan behind every block and light broadcast,
// behind "who is told that this chunk is ready", and behind "does anybody still need it".
//
// A player's view is vanilla's ChunkTrackingView.Positioned: the chunks (x, z) around its centre
// with  max(0, |dx| - 2)^2 + max(0, |dz| - 2)^2 < distance^2  - a square with its corners cut,
// one chunk wider than the view distance. There is no list of viewers per chunk to keep in step
// with every move; the three columns (centre x, centre z, distance) of all players are scanned,
// eight players a step.
//
// `margin` widens the test: 2 is the view itself (what isWithinDistance calls includeNeighbors);
// 4 is the view and two chunks around it - what has to be loaded for the view's chunks to tick
// (a chunk ticks when its neighbours are lit, and is lit when its neighbours are loaded).
#pragma once

#include <immintrin.h>

#include <bit>
#include <cstdlib>
#include <vector>

#include "base/types.hpp"

namespace server {

inline constexpr i32 kViewMargin = 2, kLoadMargin = 4;

// ChunkTrackingView.isWithinDistance, for one player.
inline bool in_view(i32 centre_x, i32 centre_z, i32 distance, i32 cx, i32 cz, i32 margin = kViewMargin) {
    const i64 dx = std::max(0, std::abs(cx - centre_x) - margin), dz = std::max(0, std::abs(cz - centre_z) - margin);
    return dx * dx + dz * dz < static_cast<i64>(distance) * distance;
}

// Appends to `inside` the players i that have (cx, cz) within their view widened to `margin`.
inline void find_in_view(const i32* xs, const i32* zs, const i32* views, u32 n, i32 cx, i32 cz, i32 margin,
                         std::vector<u32>& inside) {
    const __m256i vx = _mm256_set1_epi32(cx), vz = _mm256_set1_epi32(cz), vm = _mm256_set1_epi32(margin);
    // Far apart, a difference squared does not fit 32 bits: beyond this nothing is in view anyway
    // (a view distance is at most 32), and two of these squared still fit.
    const __m256i cap = _mm256_set1_epi32(32767), zero = _mm256_setzero_si256();
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i dx = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(xs + i)), vx));
        __m256i dz = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zs + i)), vz));
        dx = _mm256_min_epi32(_mm256_max_epi32(_mm256_sub_epi32(dx, vm), zero), cap);
        dz = _mm256_min_epi32(_mm256_max_epi32(_mm256_sub_epi32(dz, vm), zero), cap);
        const __m256i d2 = _mm256_add_epi32(_mm256_mullo_epi32(dx, dx), _mm256_mullo_epi32(dz, dz));
        const __m256i view = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(views + i));
        u32 in =
            static_cast<u32>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_mullo_epi32(view, view), d2))));
        for (; in; in &= in - 1) inside.push_back(i + static_cast<u32>(std::countr_zero(in)));
    }
    for (; i < n; ++i)
        if (in_view(xs[i], zs[i], views[i], cx, cz, margin)) inside.push_back(i);
}

// Whether any player has (cx, cz) within its view widened to `margin`.
inline bool any_in_view(const i32* xs, const i32* zs, const i32* views, u32 n, i32 cx, i32 cz, i32 margin) {
    const __m256i vx = _mm256_set1_epi32(cx), vz = _mm256_set1_epi32(cz), vm = _mm256_set1_epi32(margin);
    const __m256i cap = _mm256_set1_epi32(32767), zero = _mm256_setzero_si256();
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        __m256i dx = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(xs + i)), vx));
        __m256i dz = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zs + i)), vz));
        dx = _mm256_min_epi32(_mm256_max_epi32(_mm256_sub_epi32(dx, vm), zero), cap);
        dz = _mm256_min_epi32(_mm256_max_epi32(_mm256_sub_epi32(dz, vm), zero), cap);
        const __m256i d2 = _mm256_add_epi32(_mm256_mullo_epi32(dx, dx), _mm256_mullo_epi32(dz, dz));
        const __m256i view = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(views + i));
        if (_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(_mm256_mullo_epi32(view, view), d2)))) return true;
    }
    for (; i < n; ++i)
        if (in_view(xs[i], zs[i], views[i], cx, cz, margin)) return true;
    return false;
}

}  // namespace server
