// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen golden: what vanilla computes, written down for the unit tests of src/worldgen.
//
// One little-endian file per area. A file is a script the test replays: every record says what
// was asked and what vanilla answered, floats and doubles as their bits. The scripts are drawn
// from java.util.Random with fixed seeds, so a run writes the same bytes every time.
//
//   random.bin   the random sources: sequences of calls on each kind of source, the seeds
//                positions and names hash to, how a seed is read and how it is hidden from clients

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.OptionalLong;
import java.util.Random;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.RandomSupport;
import net.minecraft.world.level.levelgen.SingleThreadedRandomSource;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;

final class WorldgenGolden {
    /** A file in the making. */
    static final class Out {
        private final ByteArrayOutputStream bytes = new ByteArrayOutputStream();

        void u8(int v) {
            bytes.write(v);
        }

        void i32(int v) {
            for (int i = 0; i < 4; ++i) bytes.write(v >>> (8 * i));
        }

        void i64(long v) {
            for (int i = 0; i < 8; ++i) bytes.write((int) (v >>> (8 * i)));
        }

        void f32(float v) {
            i32(Float.floatToRawIntBits(v));
        }

        void f64(double v) {
            i64(Double.doubleToRawLongBits(v));
        }

        void str(String s) {
            byte[] b = s.getBytes(StandardCharsets.UTF_8);
            i32(b.length);
            bytes.writeBytes(b);
        }

        void magic(String s) {
            bytes.writeBytes(s.getBytes(StandardCharsets.US_ASCII));
        }

        int save(Path file) throws IOException {
            Files.write(file, bytes.toByteArray());
            return bytes.size();
        }
    }

    // ------------------------------------------------------------------------------- random

    static final String[] NAMES = {
        "", "a", "minecraft:aquifer", "minecraft:ore", "minecraft:bedrock_floor", "minecraft:deepslate", "minecraft:clay_bands",
        "minecraft:terrain", "octave_-7", "octave_0", "minecraft:temperature", "The quick brown fox", "é世界 🌍"};

    static final String[] SEEDS = {
        "", "   ", "0", "1", "-1", " 42 ", "+7", "-0", "9223372036854775807", "-9223372036854775808", "9223372036854775808",
        "0x10", "1e3", "12 34", "North Carolina", "seed", "Glacier", "\t-12345\n", "--1", "+", "-", "00012", "été 🌍"};

    static final long[] LONGS = {0L, 1L, -1L, 42L, Long.MIN_VALUE, Long.MAX_VALUE, 0x5DEECE66DL, -7046029254386353131L, 7640891576956012809L};

    static final int[] BOUNDS = {1, 2, 3, 5, 7, 10, 16, 100, 255, 256, 1000, 65536, 1 << 30, (1 << 30) + 1, Integer.MAX_VALUE - 1, Integer.MAX_VALUE};

    static RandomSource source(int kind, long seed) {
        return switch (kind) {
            case 0 -> new XoroshiroRandomSource(seed);
            case 1 -> new LegacyRandomSource(seed);
            case 2 -> new SingleThreadedRandomSource(seed);
            case 3 -> new WorldgenRandom(new LegacyRandomSource(seed));
            default -> new WorldgenRandom(new XoroshiroRandomSource(seed));
        };
    }

    /** One call on `r`, drawn by `script`; returns the source the script goes on with. */
    static RandomSource call(Out o, Random script, RandomSource r) {
        boolean worldgen = r instanceof WorldgenRandom;
        int op = script.nextInt(worldgen ? 20 : 16);
        o.u8(op);
        switch (op) {
            case 0 -> o.i32(r.nextInt());
            case 1 -> {
                int bound = script.nextInt(4) == 0 ? 1 + script.nextInt(Integer.MAX_VALUE) : BOUNDS[script.nextInt(BOUNDS.length)];
                o.i32(bound);
                o.i32(r.nextInt(bound));
            }
            case 2 -> o.i64(r.nextLong());
            case 3 -> o.u8(r.nextBoolean() ? 1 : 0);
            case 4 -> o.f32(r.nextFloat());
            case 5 -> o.f64(r.nextDouble());
            case 6 -> o.f64(r.nextGaussian());
            case 7 -> r = r.fork();
            case 8 -> {
                int x = script.nextInt(), y = script.nextInt(768) - 384, z = script.nextInt();
                if (script.nextBoolean()) {  // the positions a world is made of, mostly
                    x >>= 8;
                    z >>= 8;
                }
                o.i32(x);
                o.i32(y);
                o.i32(z);
                r = r.forkPositional().at(x, y, z);
            }
            case 9 -> {
                String name = NAMES[script.nextInt(NAMES.length)];
                o.str(name);
                r = r.forkPositional().fromHashOf(name);
            }
            case 10 -> {
                long seed = script.nextLong();
                o.i64(seed);
                r = r.forkPositional().fromSeed(seed);
            }
            case 11 -> {
                long seed = script.nextBoolean() ? script.nextLong() : LONGS[script.nextInt(LONGS.length)];
                o.i64(seed);
                r.setSeed(seed);
            }
            case 12 -> {
                int rounds = script.nextInt(5);
                o.i32(rounds);
                r.consumeCount(rounds);
            }
            case 13 -> {
                int min = script.nextInt(2000) - 1000, max = min + script.nextInt(1000);
                o.i32(min);
                o.i32(max);
                o.i32(r.nextIntBetweenInclusive(min, max));
            }
            case 14 -> {
                double mean = script.nextDouble() * 100 - 50, spread = script.nextDouble() * 10;
                o.f64(mean);
                o.f64(spread);
                o.f64(r.triangle(mean, spread));
            }
            case 15 -> {
                float mean = script.nextFloat() * 100 - 50, spread = script.nextFloat() * 10;
                o.f32(mean);
                o.f32(spread);
                o.f32(r.triangle(mean, spread));
            }
            case 16 -> {
                long seed = script.nextLong();
                int x = script.nextInt() >> 12, z = script.nextInt() >> 12;
                o.i64(seed);
                o.i32(x);
                o.i32(z);
                o.i64(((WorldgenRandom) r).setDecorationSeed(seed, x, z));
            }
            case 17 -> {
                long seed = script.nextLong();
                int index = script.nextInt(300), step = script.nextInt(11);
                o.i64(seed);
                o.i32(index);
                o.i32(step);
                ((WorldgenRandom) r).setFeatureSeed(seed, index, step);
            }
            case 18 -> {
                long seed = script.nextLong();
                int x = script.nextInt() >> 12, z = script.nextInt() >> 12;
                o.i64(seed);
                o.i32(x);
                o.i32(z);
                ((WorldgenRandom) r).setLargeFeatureSeed(seed, x, z);
            }
            default -> {
                long seed = script.nextLong();
                int x = script.nextInt() >> 12, z = script.nextInt() >> 12, salt = script.nextInt();
                o.i64(seed);
                o.i32(x);
                o.i32(z);
                o.i32(salt);
                ((WorldgenRandom) r).setLargeFeatureWithSalt(seed, x, z, salt);
            }
        }
        return r;
    }

    static int random(Path file) throws IOException {
        Out o = new Out();
        o.magic("WGRAND01");
        Random script = new Random(20261123L);

        // What positions, names and seeds hash to.
        o.i32(256);
        for (int i = 0; i < 256; ++i) {
            int x = script.nextInt(), y = script.nextInt(), z = script.nextInt();
            if (i < 128) {
                x >>= 10;
                y = script.nextInt(768) - 384;
                z >>= 10;
            }
            o.i32(x);
            o.i32(y);
            o.i32(z);
            o.i64(Mth.getSeed(x, y, z));
        }
        o.i32(LONGS.length + 64);
        for (int i = 0; i < LONGS.length + 64; ++i) {
            long v = i < LONGS.length ? LONGS[i] : script.nextLong();
            RandomSupport.Seed128bit wide = RandomSupport.upgradeSeedTo128bit(v);
            o.i64(v);
            o.i64(RandomSupport.mixStafford13(v));
            o.i64(wide.seedLo());
            o.i64(wide.seedHi());
            o.i64(BiomeManager.obfuscateSeed(v));
        }
        o.i32(NAMES.length);
        for (String name : NAMES) {
            RandomSupport.Seed128bit hash = RandomSupport.seedFromHashOf(name);
            o.str(name);
            o.i64(hash.seedLo());
            o.i64(hash.seedHi());
            o.i32(name.hashCode());
        }
        // How the level-seed of server.properties is read.
        o.i32(SEEDS.length);
        for (String text : SEEDS) {
            OptionalLong seed = WorldOptions.parseSeed(text);
            o.str(text);
            o.u8(seed.isPresent() ? 1 : 0);
            o.i64(seed.orElse(0));
        }

        // Calls on each kind of source, one after the other.
        final int kinds = 5, cases = 24, calls = 96;
        o.i32(kinds * cases);
        for (int kind = 0; kind < kinds; ++kind)
            for (int c = 0; c < cases; ++c) {
                long seed = c < LONGS.length ? LONGS[c] : script.nextLong();
                o.u8(kind);
                o.i64(seed);
                o.i32(calls);
                RandomSource r = source(kind, seed);
                for (int i = 0; i < calls; ++i) r = call(o, script, r);
            }
        // A positional factory asked many times: what an aquifer, a surface and an ore vein draw from.
        for (int kind = 0; kind < 2; ++kind) {
            PositionalRandomFactory factory = source(kind, 20261123L).forkPositional();
            o.i32(512);
            for (int i = 0; i < 512; ++i) {
                int x = script.nextInt() >> 10, y = script.nextInt(448) - 64, z = script.nextInt() >> 10;
                RandomSource r = factory.at(x, y, z);
                o.i32(x);
                o.i32(y);
                o.i32(z);
                o.i32(r.nextInt(10));
                o.f32(r.nextFloat());
                o.f64(r.nextDouble());
            }
        }
        return o.save(file);
    }

    interface Writer {
        int write(Path file) throws Exception;
    }

    static final java.util.Map<String, Writer> FILES = new java.util.LinkedHashMap<>();

    static {
        FILES.put("random", WorldgenGolden::random);
    }
}
