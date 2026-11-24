// SPDX-License-Identifier: GPL-3.0-or-later
// The order of a java.util.HashSet<BlockPos>, for the golden script: what src/worldgen/java_hash.* has
// to give (a tree's decorators walk such sets with a draw an element; TreeFeature.updateLeaves takes
// leaves off them, the first of the order at a time).
//
// A script of sets - positions put in, the first taken out through an iterator, the whole order read -
// each run from a number, by the same arithmetic as tests/unit/feature_test.cpp runs it, and what
// came of it folded into one number. A quarter of the scripts are the positions of something like a
// tree; the others crowd few buckets, so that buckets become trees, are split and shrink again.

import java.util.HashSet;
import java.util.Iterator;

import net.minecraft.core.BlockPos;

final class WorldgenHash {
    private WorldgenHash() {}

    static final int CASES = 800;

    static void golden(WorldgenGolden.Out o) throws Exception {
        o.i32(CASES);
        for (int c = 0; c < CASES; ++c) o.i64(run(c));
        System.err.println("worldgen: golden features: " + CASES + " scripts of hash sets");
    }

    static long order(long h, HashSet<BlockPos> set) {
        for (BlockPos p : set) h = WorldgenGolden.fnv(WorldgenGolden.fnv(WorldgenGolden.fnv(h, p.getX()), p.getY()), p.getZ());
        return WorldgenGolden.fnv(h, set.size());
    }

    static long run(int c) {
        WorldgenSurvive.Mix m = new WorldgenSurvive.Mix(0x4A485348L + c);
        int mode = c & 3;
        boolean taking = (c & 4) != 0;
        int ox = (int) (m.next() >>> 38) - (1 << 25), oy = (int) (m.next() >>> 56) - 64, oz = (int) (m.next() >>> 38) - (1 << 25);
        int modulus = 1 << (6 + (int) (m.next() >>> 61));
        int ops = 200 + (int) (m.next() >>> 53);
        HashSet<BlockPos> set = new HashSet<>();
        long h = 0xcbf29ce484222325L;
        for (int i = 0; i < ops; ++i) {
            int kind = (int) (m.next() >>> 60);
            long q = m.next();
            int a = (int) (q & 0xFFFF), b = (int) (q >>> 16 & 0xFFFF), d = (int) (q >>> 32 & 0xFFFF), e = (int) (q >>> 48 & 0xFFFF);
            boolean crowd = mode == 2 || (mode == 3 && (e & 1) != 0);
            int x, y, z;
            if (mode == 1) {
                x = ox + a % 5;
                y = oy + b % 5;
                z = oz + d % 5;
            } else if (!crowd) {
                x = ox + a % 16;
                y = oy + b % 31;
                z = oz + d % 16;
            } else {
                // One of three hashes as far as the modulus goes, and 256 positions for each - no two of them
                // with one hash (a tree would order those by their identity: by nothing).
                int want = a % 3 * 7, k = b % 256;
                y = oy + k % 8;
                z = oz + k / 8 % 8;
                x = ox + want + modulus * k - (y + z * 31) * 31;
            }
            BlockPos pos = new BlockPos(x, y, z);
            if (kind < (taking ? 8 : 11)) {
                h = WorldgenGolden.fnv(h, set.add(pos) ? 1 : 0);
            } else if (kind < 14) {
                if (set.isEmpty()) continue;
                Iterator<BlockPos> iterator = set.iterator();
                BlockPos first = iterator.next();
                iterator.remove();
                h = WorldgenGolden.fnv(WorldgenGolden.fnv(WorldgenGolden.fnv(h, first.getX()), first.getY()), first.getZ());
            } else if (kind == 14) {
                h = WorldgenGolden.fnv(h, set.contains(pos) ? 1 : 0);
            } else {
                h = order(h, set);
            }
        }
        h = order(h, set);
        HashSet<BlockPos> copy = new HashSet<>();
        copy.addAll(set);
        return order(h, copy);
    }
}
