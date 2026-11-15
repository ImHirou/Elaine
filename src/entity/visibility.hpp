// The visibility kernel of the entity tracker: one player against every entity, 64 answers per
// word. A header so that the benchmark can time it alone ("visibility scan").
#pragma once

#include <immintrin.h>

#include "base/types.hpp"

namespace entity {

// ChunkMap.TrackedEntity.updatePlayer for one player against 64 * nwords entities: bit e of the
// result is  !(dx^2 + dz^2 > min(range^2, view^2)),  the horizontal distance only. Written as
// vanilla computes it (two products, one sum, no FMA) so that a pair on the boundary falls on
// the same side.
HOT inline void visible_words(const double* RESTRICT x, const double* RESTRICT z, const double* RESTRICT range2, u32 nwords,
                              double px, double pz, double view2, u64* RESTRICT out) {
#if defined(__AVX512F__)
    const __m512d vx = _mm512_set1_pd(px), vz = _mm512_set1_pd(pz), vv = _mm512_set1_pd(view2);
    for (u32 w = 0; w < nwords; ++w) {
        u64 bits = 0;
        for (u32 i = 0; i < 8; ++i) {  // fixed trip count: the vectors stay in registers
            const usize at = static_cast<usize>(w) * 64 + i * 8;
            const __m512d dx = _mm512_sub_pd(vx, _mm512_loadu_pd(x + at));
            const __m512d dz = _mm512_sub_pd(vz, _mm512_loadu_pd(z + at));
            const __m512d d2 = _mm512_add_pd(_mm512_mul_pd(dx, dx), _mm512_mul_pd(dz, dz));
            const __m512d lim = _mm512_min_pd(_mm512_loadu_pd(range2 + at), vv);
            bits |= static_cast<u64>(_mm512_cmp_pd_mask(d2, lim, _CMP_NGT_UQ)) << (i * 8);
        }
        out[w] = bits;
    }
#else
    const __m256d vx = _mm256_set1_pd(px), vz = _mm256_set1_pd(pz), vv = _mm256_set1_pd(view2);
    for (u32 w = 0; w < nwords; ++w) {
        u64 bits = 0;
        for (u32 i = 0; i < 16; ++i) {  // fixed trip count: the vectors stay in registers
            const usize at = static_cast<usize>(w) * 64 + i * 4;
            const __m256d dx = _mm256_sub_pd(vx, _mm256_loadu_pd(x + at));
            const __m256d dz = _mm256_sub_pd(vz, _mm256_loadu_pd(z + at));
            const __m256d d2 = _mm256_add_pd(_mm256_mul_pd(dx, dx), _mm256_mul_pd(dz, dz));
            const __m256d lim = _mm256_min_pd(_mm256_loadu_pd(range2 + at), vv);
            bits |= static_cast<u64>(static_cast<u32>(_mm256_movemask_pd(_mm256_cmp_pd(d2, lim, _CMP_NGT_UQ)))) << (i * 4);
        }
        out[w] = bits;
    }
#endif
}

}  // namespace entity
