// SPDX-License-Identifier: GPL-3.0-or-later
// java.lang.Math.sin and cos, as the JVM a vanilla server runs on answers them.
//
// Java's Math.sin is not the C library's: HotSpot has routines of its own for it, and their
// results differ from glibc's in the last bit for about one argument in five hundred. A feature
// that turns a random angle into blocks - an ore's blob, a fancy oak's branch - goes by those
// bits. Neither library's sine is used here: HotSpot's is not ours to copy (GPL 2 only), and the
// C library's is another one on another machine.
//
// Two parts:
//   - a sine and a cosine of our own, in plain IEEE arithmetic (double-double: sums and products
//     with their rounding errors carried along) - the same additions and multiplications in the
//     same order on every machine, and the same again in the extractor
//     (tools/mcgen/extract/WorldgenMath.java). They give the nearest double (held to exact
//     arithmetic over a sample of the angles; the C library's does not always);
//   - for the arguments vanilla can have - an angle that comes from a float is one of 2^24 a
//     formula, and the extractor asks the JVM about every one; the angles of the search for where
//     a world begins are the same in every world - the ones where HotSpot answers the next double
//     instead: a table of them (worldgen_math.bin), so that for those arguments the answer is
//     HotSpot's, all of them (tests/unit/feature_test.cpp goes through all 2^24).
//
// An angle that comes from a double - an iceberg's - has too many values to ask about: there the
// answer is our own, which is HotSpot's but for the last bit of about one argument in nine hundred.
// (Math.pow(x, 2.0) is x * x in HotSpot, exactly: no function is needed for it.)
#pragma once

#include "base/types.hpp"

namespace worldgen::jmath {

// The sine and cosine of our own. For 0 <= x < 8: no angle of vanilla's is beyond a turn.
double sin_own(double x);
double cos_own(double x);

// Math.sin, Math.cos: HotSpot's answer wherever the extractor asked for it (the domains of
// WorldgenMath.java), our own elsewhere.
double sin(double x);
double cos(double x);

// The natural logarithm of a positive number, and a positive number to a power: each the nearest
// double to the true value. HotSpot's Math.log and Math.pow are within the last bit of that and
// not always the same double, and their arguments are doubles - there is no asking the JVM about
// all of them, as for the sines of a float. So whoever uses these must not hang on the last bit:
// the one who does, a large dripstone's height (cave.cpp), rounds the result down to a whole
// number, and takes this slow way only where that number is in doubt.
double log(double x);
double pow(double x, double y);

}  // namespace worldgen::jmath
