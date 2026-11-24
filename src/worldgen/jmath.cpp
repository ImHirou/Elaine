// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/jmath.hpp"

#include <bit>

#include "proto/gen/current.hpp"

#include PROTO_GEN_WORLDGEN_MATH_HPP

namespace worldgen::jmath {

namespace {

// A number as the sum of two doubles, the second less than half the last bit of the first.
// Every operation below is written out in the order it is done in: the extractor does the same
// ones in the same order (WorldgenMath.java), and neither side may fuse or regroup them.
struct DD {
    double hi, lo;
};

// a + b, and what the sum lost.
DD two_sum(double a, double b) {
    const double s = a + b;
    const double bb = s - a;
    return {s, (a - (s - bb)) + (b - bb)};
}
// The same where |a| >= |b|.
DD quick_two_sum(double a, double b) {
    const double s = a + b;
    return {s, b - (s - a)};
}
// a * b, and what the product lost (Dekker: both halves of each factor are 26 bits).
DD two_prod(double a, double b) {
    const double p = a * b;
    double t = 134217729.0 * a;
    const double ah = t - (t - a), al = a - ah;
    t = 134217729.0 * b;
    const double bh = t - (t - b), bl = b - bh;
    return {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
}
DD add(DD x, DD y) {
    const DD s = two_sum(x.hi, y.hi);
    return quick_two_sum(s.hi, s.lo + (x.lo + y.lo));
}
DD mul(DD x, DD y) {
    const DD p = two_prod(x.hi, y.hi);
    return quick_two_sum(p.hi, p.lo + (x.hi * y.lo + x.lo * y.hi));
}

// pi / 2 in three pieces, and its reciprocal.
constexpr double kPio2Hi = 0x1.921fb54442d18p+0, kPio2Lo = 0x1.1a62633145c07p-54, kPio2Lo2 = -0x1.f1976b7ed8fbcp-110;
constexpr double kTwoOverPi = 0x1.45f306dc9c883p-1;

// The series of the sine and the cosine: 1 / k! with its sign. The first terms as pairs, the
// rest - from x^11 and x^12 on, where a double's own precision is far more than is needed - as doubles.
constexpr DD kS3{-0x1.5555555555555p-3, -0x1.5555555555555p-57}, kS5{0x1.1111111111111p-7, 0x1.1111111111111p-63},
    kS7{-0x1.a01a01a01a01ap-13, -0x1.a01a01a01a01ap-73}, kS9{0x1.71de3a556c734p-19, -0x1.c154f8ddc6c00p-73};
constexpr double kSinTail[9] = {-0x1.ae64567f544e4p-26, 0x1.6124613a86d09p-33,  -0x1.ae7f3e733b81fp-41,
                                0x1.952c77030ad4ap-49,  -0x1.2f49b46814157p-57, 0x1.71b8ef6dcf572p-66,
                                -0x1.761b41316381ap-75, 0x1.3f3ccdd165fa9p-84,  -0x1.d1ab1c2dccea3p-94};
constexpr DD kC2{-0x1.0000000000000p-1, 0.0}, kC4{0x1.5555555555555p-5, 0x1.5555555555555p-59},
    kC6{-0x1.6c16c16c16c17p-10, 0x1.f49f49f49f49fp-65}, kC8{0x1.a01a01a01a01ap-16, 0x1.a01a01a01a01ap-76},
    kC10{-0x1.27e4fb7789f5cp-22, -0x1.cbbc05b4fa99ap-76};
constexpr double kCosTail[9] = {0x1.1eed8eff8d898p-29,  -0x1.93974a8c07c9dp-37, 0x1.ae7f3e733b81fp-45,
                                -0x1.6827863b97d97p-53, 0x1.e542ba4020225p-62,  -0x1.0ce396db7f853p-70,
                                0x1.f2cf01972f578p-80,  -0x1.88e85fc6a4e5ap-89, 0x1.0a18a2635085dp-98};

// x less the nearest multiple of pi / 2; `n`: which multiple.
DD reduce(double x, i32& n) {
    n = static_cast<i32>(x * kTwoOverPi + 0.5);
    if (n == 0) return {x, 0.0};
    const double m = static_cast<double>(n);
    const DD t = two_prod(m, kPio2Hi);
    const DD d = two_sum(x, -t.hi);
    const double low = ((d.lo - t.lo) - m * kPio2Lo) - m * kPio2Lo2;
    return two_sum(d.hi, low);
}

// sin r = r (1 + r^2 (S3 + r^2 (S5 + r^2 (S7 + r^2 (S9 + r^2 (S11 + ...)))))), |r| <= pi / 4.
double sin_kernel(DD r) {
    const DD r2 = mul(r, r);
    const double z = r2.hi;
    double tail = kSinTail[8];
    for (i32 i = 7; i >= 0; --i) tail = tail * z + kSinTail[i];
    DD p{tail, 0.0};
    p = add(mul(p, r2), kS9);
    p = add(mul(p, r2), kS7);
    p = add(mul(p, r2), kS5);
    p = add(mul(p, r2), kS3);
    p = mul(p, r2);
    p = add(mul(p, r), r);
    return p.hi;
}

// cos r = 1 + r^2 (C2 + r^2 (C4 + ... (C10 + r^2 (C12 + ...)))).
double cos_kernel(DD r) {
    const DD r2 = mul(r, r);
    const double z = r2.hi;
    double tail = kCosTail[8];
    for (i32 i = 7; i >= 0; --i) tail = tail * z + kCosTail[i];
    DD p{tail, 0.0};
    p = add(mul(p, r2), kC10);
    p = add(mul(p, r2), kC8);
    p = add(mul(p, r2), kC6);
    p = add(mul(p, r2), kC4);
    p = add(mul(p, r2), kC2);
    p = mul(p, r2);
    p = add(p, DD{1.0, 0.0});
    return p.hi;
}

// worldgen_math.bin: "WGMATH01", how many arguments the sine and the cosine have a correction
// for; then for each of the two its arguments - a double's bits, in order - and what is to be
// added to the bits of our own answer, each list padded to eight bytes.
namespace table = proto::gen::worldgen::math;

constexpr u32 word(usize at) {
    return static_cast<u32>(table::kCorrections[at]) | static_cast<u32>(table::kCorrections[at + 1]) << 8 |
           static_cast<u32>(table::kCorrections[at + 2]) << 16 | static_cast<u32>(table::kCorrections[at + 3]) << 24;
}
constexpr usize padded(usize n) { return (n + 7) & ~usize{7}; }
constexpr u32 kSines = word(8), kCosines = word(12);
constexpr usize kSineKeys = 16, kSineDeltas = kSineKeys + usize{kSines} * 8;
constexpr usize kCosineKeys = kSineDeltas + padded(kSines), kCosineDeltas = kCosineKeys + usize{kCosines} * 8;
static_assert(sizeof(table::kCorrections) == kCosineDeltas + padded(kCosines), "worldgen_math.bin is not what jmath.cpp reads");

double corrected(double own, double x, usize keys, usize deltas, u32 count) {
    const u64 key = std::bit_cast<u64>(x);
    u32 lo = 0, hi = count;
    while (lo < hi) {
        const u32 mid = (lo + hi) / 2;
        if (load_le<u64>(table::kCorrections + keys + usize{mid} * 8) < key)
            lo = mid + 1;
        else
            hi = mid;
    }
    if (lo == count || load_le<u64>(table::kCorrections + keys + usize{lo} * 8) != key) return own;
    const i64 delta = static_cast<i8>(table::kCorrections[deltas + lo]);
    return std::bit_cast<double>(std::bit_cast<u64>(own) + static_cast<u64>(delta));
}

}  // namespace

double sin_own(double x) {
    i32 n;
    const DD r = reduce(x, n);
    switch (n & 3) {
        case 0: return sin_kernel(r);
        case 1: return cos_kernel(r);
        case 2: return -sin_kernel(r);
        default: return -cos_kernel(r);
    }
}

double cos_own(double x) {
    i32 n;
    const DD r = reduce(x, n);
    switch (n & 3) {
        case 0: return cos_kernel(r);
        case 1: return -sin_kernel(r);
        case 2: return -cos_kernel(r);
        default: return sin_kernel(r);
    }
}

double sin(double x) { return corrected(sin_own(x), x, kSineKeys, kSineDeltas, kSines); }
double cos(double x) { return corrected(cos_own(x), x, kCosineKeys, kCosineDeltas, kCosines); }

}  // namespace worldgen::jmath
