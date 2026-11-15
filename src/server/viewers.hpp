// Which players see a chunk: the scan behind every block and light broadcast.
//
// A player's view is the square of chunks within `view` of the chunk they stand in. There is no
// list of viewers per chunk to keep in step with every move; instead the three columns (centre x,
// centre z, radius) of all players are scanned, eight players per step. With 500 players that is
// ~63 steps per changed chunk (bench "viewer scan").
#pragma once

#include <immintrin.h>

#include <bit>
#include <cstdlib>
#include <vector>

#include "base/types.hpp"

namespace server {

// Appends to `inside` the indices i with max(|xs[i] - cx|, |zs[i] - cz|) <= views[i], and to `edge`
// those where it is == views[i] (the chunk is on the border of that player's view).
inline void find_viewers(const i32* xs, const i32* zs, const i32* views, u32 n, i32 cx, i32 cz, std::vector<u32>& inside,
                         std::vector<u32>& edge) {
    const __m256i vx = _mm256_set1_epi32(cx), vz = _mm256_set1_epi32(cz);
    u32 i = 0;
    for (; i + 8 <= n; i += 8) {
        const __m256i dx = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(xs + i)), vx));
        const __m256i dz = _mm256_abs_epi32(_mm256_sub_epi32(_mm256_loadu_si256(reinterpret_cast<const __m256i*>(zs + i)), vz));
        const __m256i d = _mm256_max_epi32(dx, dz);
        const __m256i view = _mm256_loadu_si256(reinterpret_cast<const __m256i*>(views + i));
        u32 in = ~static_cast<u32>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpgt_epi32(d, view)))) & 0xFF;
        u32 on = static_cast<u32>(_mm256_movemask_ps(_mm256_castsi256_ps(_mm256_cmpeq_epi32(d, view))));
        for (; in; in &= in - 1) inside.push_back(i + static_cast<u32>(std::countr_zero(in)));
        for (; on; on &= on - 1) edge.push_back(i + static_cast<u32>(std::countr_zero(on)));
    }
    for (; i < n; ++i) {
        const i32 d = std::max(std::abs(xs[i] - cx), std::abs(zs[i] - cz));
        if (d <= views[i]) inside.push_back(i);
        if (d == views[i]) edge.push_back(i);
    }
}

}  // namespace server
