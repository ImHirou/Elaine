// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen golden: what vanilla computes, written down for the unit tests of src/worldgen.
//
// One little-endian file per area. A file is a script the test replays: every record says what
// was asked and what vanilla answered, floats and doubles as their bits. The scripts are drawn
// from java.util.Random with fixed seeds, so a run writes the same bytes every time.
//
//   random.bin   the random sources: sequences of calls on each kind of source, the seeds
//                positions and names hash to, how a seed is read and how it is hidden from clients
//   density.bin  the density functions: every function vanilla asks a sampler of, at points and
//                over volumes of the shapes a generator samples in - each by itself, and then in
//                sequences on one context with caches, where an answer depends on the questions before
//   climate.bin  which biome a climate is: searches one after the other - an answer depends on
//                the one before where two biomes are equally near - and the biomes of whole chunks
//   terrain.bin  whole chunks as vanilla's `terrain` status makes them, after each of its steps:
//                the blocks (a few chunks whole, the rest as a hash), the heightmaps generation
//                goes by, and what is marked to be let flow
//   noise.bin    every noise of the overworld for one seed: what the seed made of each layer
//                (permutation, offsets), its value at points, and what it adds to a buffer over
//                a volume - the two ways vanilla samples a noise, which are not the same bits

import java.io.ByteArrayOutputStream;
import java.io.IOException;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.OptionalLong;
import java.util.Random;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.Registries;
import net.minecraft.util.Mth;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.biome.Biome;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeResolver;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSource;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.LevelHeightAccessor;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.LevelChunkSection;
import net.minecraft.world.level.chunk.PalettedContainerFactory;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.UpgradeData;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.levelgen.Aquifer;
import net.minecraft.world.level.levelgen.Beardifier;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.LegacyRandomSource;
import net.minecraft.world.level.levelgen.NoiseBasedChunkGenerator;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.NoiseSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.material.rule.MaterialRule;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.PositionalRandomFactory;
import net.minecraft.world.level.levelgen.RandomSupport;
import net.minecraft.world.level.levelgen.SingleThreadedRandomSource;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.WorldgenRandom;
import net.minecraft.world.level.levelgen.XoroshiroRandomSource;
import net.minecraft.world.level.levelgen.densityfunction.DensityBuffer;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.densityfunction.DensityVolume;
import net.minecraft.world.level.levelgen.densityfunction.SamplerContext;
import net.minecraft.world.level.levelgen.synth.Noise;
import net.minecraft.world.level.levelgen.synth.SmearedPerlinNoise;

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

    static int random(Path file, WorldgenRef.World world) throws IOException {
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

    // ------------------------------------------------------------------------------- noise

    /** Scales a density function gives a noise: 1, a quarter, thirds, what the terrain's blend uses. */
    static final double[] SCALES = {1.0, 0.25, 4.0 / 3.0, 1500.0, 684.412 * 0.25, 0.3, 2.0, 25.0};

    /** What a test fills a buffer with before a noise is added to it: any numbers, the same on both sides. */
    static float prefill(int kind, int index) {
        return kind == 0 ? 0f : (float) index * 0.125f - 3f;
    }

    static DensityVolume volume(Random script, int shape) {
        // Where a world is: near its middle, mostly; at its border, where a noise wraps, now and then.
        int far = script.nextInt(4) == 0 ? 29_999_000 : 20_000;
        int x = script.nextInt(2 * far) - far, y = script.nextInt(384) - 64, z = script.nextInt(2 * far) - far;
        return switch (shape) {
            case 0 -> new DensityVolume(4, 12, 3, x, y, z, 1, 1, 1);           // blocks
            case 1 -> new DensityVolume(3, 7, 3, x & ~3, y & ~7, z & ~3, 4, 8, 4);  // the corners of cells
            default -> new DensityVolume(2, 1, 2, x & ~3, 0, z & ~3, 4, 1, 4);      // a slice
        };
    }

    static int noise(Path file, WorldgenRef.World world) throws Exception {
        Out o = new Out();
        o.magic("WGNOIS01");
        o.i64(world.seed());
        Random script = new Random(20261124L);
        o.i32(world.noises().size());
        for (java.util.Map.Entry<String, Noise> e : world.noises().entrySet()) {
            Noise noise = e.getValue();
            o.str(e.getKey());
            // What the seed made of it: every layer's permutation and offsets.
            Object[] layers = (Object[]) WorldgenRef.field(noise, "layers");
            o.u8(noise.getClass().getName().endsWith("SmearedPerlin") ? 1 : 0);
            o.i32(layers.length);
            for (Object layer : layers) {
                Object perlin = WorldgenRef.field(layer, "noise");
                o.f64((Double) WorldgenRef.field(layer, "frequency"));
                o.f32((Float) WorldgenRef.field(layer, "amplitude"));
                o.f64(perlin instanceof SmearedPerlinNoise ? (Double) WorldgenRef.field(perlin, "fudgeYScale") : 0.0);
                o.f64((Double) WorldgenRef.field(perlin, "offsetX"));
                o.f64((Double) WorldgenRef.field(perlin, "offsetY"));
                o.f64((Double) WorldgenRef.field(perlin, "offsetZ"));
                for (byte b : (byte[]) WorldgenRef.field(perlin, "perms")) o.u8(b);
            }
            // Its value at points: fractions, whole blocks times a scale, and far out where it wraps.
            final int points = 48;
            o.i32(points);
            for (int i = 0; i < points; ++i) {
                double x, y, z;
                switch (i % 4) {
                    case 0 -> {
                        x = script.nextDouble() * 2000 - 1000;
                        y = script.nextDouble() * 512 - 128;
                        z = script.nextDouble() * 2000 - 1000;
                    }
                    case 1 -> {
                        double scale = SCALES[script.nextInt(SCALES.length)];
                        x = (script.nextInt(200000) - 100000) * scale;
                        y = (script.nextInt(384) - 64) * scale;
                        z = (script.nextInt(200000) - 100000) * scale;
                    }
                    case 2 -> {
                        x = (script.nextBoolean() ? 1 : -1) * (2.9e7 + script.nextDouble() * 1e7);
                        y = script.nextDouble() * 4e7 - 2e7;
                        z = (script.nextBoolean() ? 1 : -1) * (1.6e7 + script.nextDouble() * 1e6);
                    }
                    default -> {
                        x = script.nextInt(64) - 32;
                        y = script.nextInt(64) - 32;
                        z = script.nextInt(64) - 32;
                    }
                }
                o.f64(x);
                o.f64(y);
                o.f64(z);
                o.f32(noise.get(x, y, z));
                o.f32(noise.get(x, z));
            }
            // ...and added to a buffer over a volume, which is how a chunk is filled.
            final int volumes = 3;
            o.i32(volumes);
            for (int shape = 0; shape < volumes; ++shape) {
                DensityVolume v = volume(script, shape);
                double xzScale = SCALES[script.nextInt(SCALES.length)], yScale = SCALES[script.nextInt(SCALES.length)];
                float amplitude = script.nextBoolean() ? 1f : script.nextFloat() * 4f;
                int kind = script.nextInt(2);
                for (int n : new int[] {v.sizeX(), v.sizeY(), v.sizeZ(), v.minBlockX(), v.minBlockY(), v.minBlockZ(), v.stepBlockX(), v.stepBlockY(), v.stepBlockZ()}) o.i32(n);
                o.f64(xzScale);
                o.f64(yScale);
                o.f32(amplitude);
                o.u8(kind);
                DensityBuffer buffer = DensityBuffer.createUnpooled(v.size());
                for (int i = 0; i < v.size(); ++i) buffer.set(i, prefill(kind, i));
                noise.addToVolume(buffer, v, xzScale, yScale, amplitude);
                for (int i = 0; i < v.size(); ++i) o.f32(buffer.get(i));
            }
        }
        return o.save(file);
    }

    // ------------------------------------------------------------------------------- density

    /** One question of a density script, and vanilla's answer. */
    static void ask(Out o, Random script, java.util.List<DensitySampler> roots, int root, SamplerContext context, DensityVolume volume) {
        DensitySampler sampler = roots.get(root);
        o.i32(root);
        if (volume == null) {
            // A point: anywhere, on a corner of the cells (where an interpolated function is its
            // input), or far out.
            int x = script.nextInt(100000) - 50000, y = script.nextInt(384) - 64, z = script.nextInt(100000) - 50000;
            switch (script.nextInt(4)) {
                case 0 -> {
                    x &= ~3;
                    y &= ~7;
                    z &= ~3;
                }
                case 1 -> {
                    x = (script.nextBoolean() ? 1 : -1) * (29_000_000 + script.nextInt(999_000));
                    z += script.nextInt(20_000_000);
                }
                default -> { }
            }
            o.u8(0);
            o.i32(x);
            o.i32(y);
            o.i32(z);
            o.f32(sampler.sampleValue(context, x, y, z));
            return;
        }
        o.u8(1);
        for (int n : new int[] {volume.sizeX(), volume.sizeY(), volume.sizeZ(), volume.minBlockX(), volume.minBlockY(), volume.minBlockZ(),
                                volume.stepBlockX(), volume.stepBlockY(), volume.stepBlockZ()}) o.i32(n);
        DensityBuffer buffer = DensityBuffer.createUnpooled(volume.size());
        sampler.sampleVolume(context, buffer, volume);
        for (int i = 0; i < volume.size(); ++i) o.f32(buffer.get(i));
    }

    /** The shapes a generator samples in: corners of cells, blocks, the quarts of biomes, layers, single blocks. */
    static DensityVolume shape(Random script, int kind, int chunkX, int chunkZ) {
        int x = chunkX * 16, z = chunkZ * 16, y = (script.nextInt(40) - 8) * 8;
        return switch (kind) {
            case 0 -> new DensityVolume(5, 13, 5, x, y, z, 4, 8, 4);                 // the corners of a chunk's cells
            case 1 -> new DensityVolume(4, 10, 4, x + script.nextInt(13), y + script.nextInt(8), z + script.nextInt(13), 1, 1, 1);
            case 2 -> new DensityVolume(4, 12, 4, x, y & ~3, z, 4, 4, 4);            // quarts
            case 3 -> new DensityVolume(16, 1, 16, x, 0, z, 1, 1, 1);                // a layer of blocks
            case 4 -> new DensityVolume(5, 1, 5, x, 0, z, 4, 1, 4);                  // ...of cell corners
            case 5 -> new DensityVolume(1, 1, 1, x + script.nextInt(16), y + script.nextInt(8), z + script.nextInt(16), 1, 1, 1);
            case 6 -> new DensityVolume(2, 1, 2, x, 0, z, 16, 1, 16);                // the corners of a chunk
            default -> new DensityVolume(1, 24, 1, x + script.nextInt(16), -64, z + script.nextInt(16), 1, 8, 1);  // a column
        };
    }

    static int density(Path file, WorldgenRef.World world) throws Exception {
        Out o = new Out();
        o.magic("WGDENS01");
        o.i64(world.seed());
        Random script = new Random(20261125L);
        java.util.List<DensitySampler> roots = new java.util.ArrayList<>(world.samplers().values());
        o.i32(roots.size());
        for (String name : world.samplers().keySet()) o.str(name);

        // Every function by itself, nothing remembered between two questions: points, then one of each shape.
        o.i32(1 + 1 + 8 + 1);
        o.u8(0);
        o.i32(roots.size() * 40);
        for (int root = 0; root < roots.size(); ++root)
            for (int i = 0; i < 40; ++i) ask(o, script, roots, root, SamplerContext.EMPTY_UNCACHED, null);
        o.u8(0);
        o.i32(roots.size() * 8);
        for (int root = 0; root < roots.size(); ++root)
            for (int kind = 0; kind < 8; ++kind)
                ask(o, script, roots, root, SamplerContext.EMPTY_UNCACHED, shape(script, kind, script.nextInt(4000) - 2000, script.nextInt(4000) - 2000));
        // A context with caches, as a chunk's has: questions of every kind about one neighbourhood,
        // one after the other - what a cache answers depends on what it was asked before.
        for (int session = 0; session < 8; ++session) {
            SamplerContext context = SamplerContext.builder().enableCaches().build();
            int chunkX = script.nextInt(2000) - 1000, chunkZ = script.nextInt(2000) - 1000;
            final int questions = 48;
            o.u8(1);
            o.i32(questions);
            for (int i = 0; i < questions; ++i) {
                int root = script.nextInt(roots.size());
                if (script.nextInt(3) == 0) {
                    ask(o, script, roots, root, context, shape(script, script.nextInt(8), chunkX + script.nextInt(2), chunkZ + script.nextInt(2)));
                } else {
                    // A point of the neighbourhood: often one a volume asked before has.
                    int x = chunkX * 16 + script.nextInt(32), y = (script.nextInt(40) - 8) * 8 + script.nextInt(8), z = chunkZ * 16 + script.nextInt(32);
                    if (script.nextBoolean()) {
                        x &= ~3;
                        z &= ~3;
                        if (script.nextBoolean()) y &= ~7;
                    }
                    if (script.nextInt(4) == 0) y = 0;
                    o.i32(root);
                    o.u8(0);
                    o.i32(x);
                    o.i32(y);
                    o.i32(z);
                    o.f32(roots.get(root).sampleValue(context, x, y, z));
                }
            }
        }
        // The terrain itself over two slabs of a chunk, as a chunk asks for it.
        SamplerContext context = SamplerContext.builder().enableCaches().build();
        int terrain = new java.util.ArrayList<>(world.samplers().keySet()).indexOf("router/finalDensity");
        o.u8(1);
        o.i32(2);
        ask(o, script, roots, terrain, context, new DensityVolume(16, 48, 16, 1600, -64, -320, 1, 1, 1));
        ask(o, script, roots, terrain, context, new DensityVolume(16, 48, 16, 1600, 40, -320, 1, 1, 1));
        return o.save(file);
    }

    // ------------------------------------------------------------------------------- climate

    /** Runs `body` on a thread of its own: Climate.RTree remembers a thread's last answer. */
    static void fresh(WorldgenRef.Visitor body) throws Exception {
        Exception[] failed = new Exception[1];
        Thread t = new Thread(() -> {
            try {
                body.visit(null);
            } catch (Exception e) {
                failed[0] = e;
            }
        });
        t.start();
        t.join();
        if (failed[0] != null) throw failed[0];
    }

    static int climate(Path file, WorldgenRef.World world) throws Exception {
        Out o = new Out();
        o.magic("WGCLIM01");
        o.i64(world.seed());
        Random script = new Random(20261126L);
        Holder<MultiNoiseBiomeSourceParameterList> preset =
            world.registries().lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
        Climate.ParameterList<Holder<Biome>> list = preset.value().parameters();
        // The values the faces of the boxes lie at: a climate exactly on one is in two boxes.
        java.util.TreeSet<Long> faces = new java.util.TreeSet<>();
        for (var pair : list.values())
            for (Climate.Parameter p : new Climate.Parameter[] {pair.getFirst().temperature(), pair.getFirst().humidity(), pair.getFirst().continentalness(),
                                                                pair.getFirst().erosion(), pair.getFirst().depth(), pair.getFirst().weirdness()}) {
                faces.add(p.min());
                faces.add(p.max());
            }
        Long[] face = faces.toArray(new Long[0]);
        // Biomes are written as a byte each: their place in the names that come first.
        java.util.List<String> names = new java.util.ArrayList<>();
        for (var pair : list.values())
            if (!names.contains(pair.getSecond().getRegisteredName())) names.add(pair.getSecond().getRegisteredName());
        o.i32(names.size());
        for (String name : names) o.str(name);

        // Searches one after the other, each sequence on a thread that has searched nothing: a
        // climate that wanders, and lands on a face now and then.
        final int sequences = 24, steps = 96;
        o.i32(sequences);
        for (int s = 0; s < sequences; ++s) {
            o.i32(steps);
            fresh(unused -> {
                float[] c = new float[6];
                for (int i = 0; i < 6; ++i) c[i] = script.nextFloat() * 2.4f - 1.2f;
                c[4] = script.nextFloat() * 1.2f - 0.1f;  // depth: from above the ground to far below it
                for (int step = 0; step < steps; ++step) {
                    for (int i = 0; i < 6; ++i) {
                        c[i] += (script.nextFloat() - 0.5f) * (script.nextInt(8) == 0 ? 0.3f : 0.01f);
                        if (script.nextInt(5) == 0) c[i] = (float) face[script.nextInt(face.length)] / 10000.0f;
                    }
                    for (float v : c) o.f32(v);
                    o.u8(names.indexOf(list.findValue(Climate.target(c[0], c[1], c[2], c[3], c[4], c[5])).getRegisteredName()));
                }
            });
        }

        // The biomes of whole chunks, asked as a chunk asks (ChunkGenerator.doCreateBiomes): the six
        // functions over the chunk's cells on a context of their own, then cell by cell.
        MultiNoiseBiomeSource source = MultiNoiseBiomeSource.createFromPreset(preset);
        final int chunks = 40, sections = 24, minSection = -4;
        java.util.Set<String> seen = new java.util.TreeSet<>();
        o.i32(chunks);
        o.i32(minSection * 16);
        o.i32(sections);
        for (int n = 0; n < chunks; ++n) {
            int cx = script.nextInt(6000) - 3000, cz = script.nextInt(6000) - 3000;
            o.i32(cx);
            o.i32(cz);
            fresh(unused -> {
                Climate.Sampler sampler = world.state().createClimateSampler(SamplerContext.builder().enableCaches().build());
                BiomeResolver resolver = source.createResolverForChunk(sampler, cx * 4, minSection * 4, cz * 4, 4, sections * 4, 4);
                for (int section = minSection; section < minSection + sections; ++section)
                    for (int x = 0; x < 4; ++x)
                        for (int y = 0; y < 4; ++y)
                            for (int z = 0; z < 4; ++z) {
                                String name = resolver.getNoiseBiome(cx * 4 + x, section * 4 + y, cz * 4 + z).getRegisteredName();
                                seen.add(name);
                                o.u8(names.indexOf(name));
                            }
            });
        }
        System.err.println("worldgen: golden climate: " + seen.size() + " biomes in " + chunks + " chunks");
        return o.save(file);
    }

    // ------------------------------------------------------------------------------- terrain

    /** A chunk made the way ChunkStatus.TERRAIN makes it, step by step - with vanilla's own methods. */
    static final class ChunkMaker {
        final WorldgenRef.World world;
        final NoiseBasedChunkGenerator generator;
        final java.lang.reflect.Method doFill, doCreateBiomes, buildSurface;
        final Aquifer.FluidPicker fluidPicker;
        final PalettedContainerFactory containers;
        final MultiNoiseBiomeSource biomeSource;

        ChunkMaker(WorldgenRef.World world) throws Exception {
            this.world = world;
            var settings = world.registries().lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD);
            var preset = world.registries().lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD);
            biomeSource = MultiNoiseBiomeSource.createFromPreset(preset);
            generator = new NoiseBasedChunkGenerator(biomeSource, settings);
            doFill = NoiseBasedChunkGenerator.class.getDeclaredMethod("doFill", NoiseChunk.class, ChunkAccess.class);
            doFill.setAccessible(true);
            doCreateBiomes = net.minecraft.world.level.chunk.ChunkGenerator.class.getDeclaredMethod("doCreateBiomes", Blender.class, RandomState.class, ChunkAccess.class);
            doCreateBiomes.setAccessible(true);
            buildSurface = NoiseBasedChunkGenerator.class.getDeclaredMethod("buildSurface", ChunkAccess.class, NoiseChunk.class, RandomState.class,
                BiomeManager.class, java.util.Set.class, MaterialRule.class);
            buildSurface.setAccessible(true);
            fluidPicker = (Aquifer.FluidPicker) ((java.util.function.Supplier<?>) WorldgenRef.field(generator, "globalFluidPicker")).get();
            containers = PalettedContainerFactory.create(world.registries());
        }

        ProtoChunk chunk(int cx, int cz) {
            NoiseSettings noise = world.settings().noiseSettings();
            return new ProtoChunk(new ChunkPos(cx, cz), UpgradeData.EMPTY, LevelHeightAccessor.create(noise.minY(), noise.height()), containers, null);
        }

        /**
         * The chunk at (cx, cz) and the eight around it, each with its biomes as the `biomes` status
         * fills them - on a thread that has searched for no biome before. [dx + 1][dz + 1].
         */
        ProtoChunk[][] neighbourhood(int cx, int cz) throws Exception {
            ProtoChunk[][] chunks = new ProtoChunk[3][3];
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    ProtoChunk chunk = chunk(cx + dx, cz + dz);
                    fresh(unused -> doCreateBiomes.invoke(generator, Blender.empty(), world.state(), chunk));
                    chunk.setPersistedStatus(ChunkStatus.BIOMES);  // (what a chunk is while its terrain is made)
                    chunks[dx + 1][dz + 1] = chunk;
                }
            return chunks;
        }

        /** What a WorldGenRegion over those nine is to the biome of a block, and the biomes that are in them. */
        BiomeManager biomeManager(ProtoChunk[][] chunks, int cx, int cz) {
            return new BiomeManager((qx, qy, qz) -> chunks[(qx >> 2) - cx + 1][(qz >> 2) - cz + 1].getNoiseBiome(qx, qy, qz),
                BiomeManager.obfuscateSeed(world.seed()));
        }

        java.util.Set<Holder<Biome>> possibleBiomes(ProtoChunk[][] chunks) {
            java.util.Set<Holder<Biome>> biomes = new java.util.HashSet<>();
            for (ProtoChunk[] row : chunks)
                for (ProtoChunk chunk : row)
                    for (LevelChunkSection section : chunk.getSections()) section.getBiomes().getAll(biomes::add);
            return biomes;
        }

        /** NoiseBasedChunkGenerator.createNoiseChunk, where no structure is near and no old chunk to blend with. */
        NoiseChunk noiseChunk(ProtoChunk chunk) {
            NoiseSettings noise = world.settings().noiseSettings();
            DensityVolume volume = new DensityVolume(16, noise.height(), 16, chunk.getPos().getMinBlockX(), noise.minY(), chunk.getPos().getMinBlockZ());
            return new NoiseChunk(world.state(), Beardifier.EMPTY, world.settings(), fluidPicker, Blender.empty(), volume);
        }
    }

    static long fnv(long h, int v) {
        for (int i = 0; i < 4; ++i) h = (h ^ ((v >>> (8 * i)) & 0xFF)) * 0x100000001b3L;
        return h;
    }

    /** A chunk as it stands: its blocks, the two heightmaps generation goes by, what is marked to be let flow. */
    static void stage(Out o, ProtoChunk chunk, boolean whole) throws IOException {
        int sections = chunk.getSectionsCount(), minY = chunk.getMinY();
        byte[] blocks = new byte[sections * 4096 * 2];
        long hash = 0xcbf29ce484222325L;
        int at = 0;
        for (int s = 0; s < sections; ++s) {
            LevelChunkSection section = chunk.getSection(s);
            for (int y = 0; y < 16; ++y)
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x) {
                        int id = Block.getId(section.getBlockState(x, y, z));
                        blocks[at++] = (byte) id;
                        blocks[at++] = (byte) (id >>> 8);
                        hash = fnv(hash, id);
                    }
        }
        o.i64(hash);
        long heights = 0xcbf29ce484222325L;
        for (Heightmap.Types type : new Heightmap.Types[] {Heightmap.Types.WORLD_SURFACE_WG, Heightmap.Types.OCEAN_FLOOR_WG}) {
            Heightmap map = chunk.getOrCreateHeightmapUnprimed(type);
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) {
                    int first = map.getFirstAvailable(x, z);
                    heights = fnv(heights, first);
                    if (whole) o.i32(first);
                }
        }
        o.i64(heights);
        long marks = 0xcbf29ce484222325L;
        int count = 0;
        it.unimi.dsi.fastutil.shorts.ShortList[] lists = chunk.getPostProcessing();
        for (int s = 0; s < lists.length; ++s)
            if (lists[s] != null) count += lists[s].size();
        o.i32(count);
        for (int s = 0; s < lists.length; ++s) {
            if (lists[s] == null) continue;
            for (short packed : lists[s]) {
                int mark = s << 12 | (packed & 0xFFF);
                marks = fnv(marks, mark);
                if (whole) o.i32(mark);
            }
        }
        o.i64(marks);
        if (whole) {
            java.util.zip.Deflater deflater = new java.util.zip.Deflater(9);
            deflater.setInput(blocks);
            deflater.finish();
            byte[] packed = new byte[blocks.length];
            int n = deflater.deflate(packed);
            deflater.end();
            o.i32(n);
            for (int i = 0; i < n; ++i) o.u8(packed[i]);
        }
    }

    /** Chunks that have each of the overworld's biomes somewhere in them - on the ground or below it - as they are found on a grid, nearest first. */
    static java.util.Map<String, java.util.List<int[]>> chunksOfEveryBiome(ChunkMaker maker) {
        Climate.Sampler sampler = maker.world.state().createClimateSampler(SamplerContext.EMPTY_UNCACHED);
        java.util.Map<String, java.util.List<int[]>> found = new java.util.TreeMap<>();
        int wanted = maker.biomeSource.possibleBiomes().size();
        for (int ring = 0; ring <= 96; ++ring)
            for (int gx = -ring; gx <= ring; ++gx)
                for (int gz = -ring; gz <= ring; ++gz) {
                    if (Math.max(Math.abs(gx), Math.abs(gz)) != ring) continue;
                    for (int qy : new int[] {16, 26, -8}) {  // y = 64, 104, -32
                        String name = maker.biomeSource.getNoiseBiome(sampler.sample(gx * 48, qy, gz * 48)).getRegisteredName();
                        java.util.List<int[]> chunks = found.computeIfAbsent(name, unused -> new java.util.ArrayList<>());
                        int[] chunk = {(gx * 48) >> 2, (gz * 48) >> 2};
                        if (chunks.size() < 40 && (chunks.isEmpty() || !java.util.Arrays.equals(chunks.getLast(), chunk))) chunks.add(chunk);
                    }
                }
        if (found.size() != wanted) throw new IllegalStateException(found.size() + " of " + wanted + " biomes were found on the grid");
        return found;
    }

    /** The first free block of every column (WORLD_SURFACE_WG). */
    static int[] surface(ProtoChunk chunk) {
        Heightmap map = chunk.getOrCreateHeightmapUnprimed(Heightmap.Types.WORLD_SURFACE_WG);
        int[] heights = new int[256];
        for (int i = 0; i < 256; ++i) heights[i] = map.getFirstAvailable(i & 15, i >> 4);
        return heights;
    }

    /**
     * A chunk through the steps of the `terrain` status, each written as it ends. Returns in how many
     * columns the surface step put blocks on top of the terrain: icebergs, the pillars of the eroded badlands.
     */
    static int make(ChunkMaker maker, Out o, int cx, int cz, boolean whole) throws Exception {
        MaterialRule rule = maker.world.settings().materialRule().value();
        ProtoChunk[][] around = maker.neighbourhood(cx, cz);
        ProtoChunk chunk = around[1][1];
        NoiseChunk noiseChunk = maker.noiseChunk(chunk);
        maker.doFill.invoke(maker.generator, noiseChunk, chunk);
        if (o != null) stage(o, chunk, whole);
        int[] before = surface(chunk);
        maker.buildSurface.invoke(maker.generator, chunk, noiseChunk, maker.world.state(), maker.biomeManager(around, cx, cz), maker.possibleBiomes(around), rule);
        if (o != null) stage(o, chunk, whole);
        int[] after = surface(chunk);
        noiseChunk.close();
        int raised = 0;
        for (int i = 0; i < 256; ++i) raised += after[i] > before[i] ? 1 : 0;
        return raised;
    }

    static int terrain(Path file, WorldgenRef.World world) throws Exception {
        Out o = new Out();
        o.magic("WGTERR02");
        o.i64(world.seed());
        Random script = new Random(20261127L);
        ChunkMaker maker = new ChunkMaker(world);
        // Some chunks anywhere, and one for every biome there is: each biome has a surface of its own.
        java.util.List<int[]> chunks = new java.util.ArrayList<>();
        final int whole = 4, anywhere = 24;
        for (int n = 0; n < anywhere; ++n) chunks.add(new int[] {script.nextInt(4000) - 2000, script.nextInt(4000) - 2000});
        java.util.Map<String, java.util.List<int[]>> every = chunksOfEveryBiome(maker);
        for (java.util.List<int[]> of : every.values()) chunks.add(of.getFirst());
        // And of the biomes whose surface adds to the terrain, chunks where it does.
        StringBuilder added = new StringBuilder();
        for (String name : new String[] {"minecraft:frozen_ocean", "minecraft:deep_frozen_ocean", "minecraft:eroded_badlands"}) {
            int took = 0;
            java.util.List<int[]> of = every.get(name);
            for (int n = 1; n < of.size() && took < 4; ++n) {
                if (make(maker, null, of.get(n)[0], of.get(n)[1], false) < 12) continue;
                chunks.add(of.get(n));
                ++took;
            }
            added.append(' ').append(name).append(' ').append(took);
        }
        System.err.println("worldgen: golden terrain: " + chunks.size() + " chunks, " + every.size() + " biomes; chunks whose surface is built up:" + added);
        o.i32(chunks.size());
        o.i32(2);  // stages: after doFill, after buildSurface
        for (int n = 0; n < chunks.size(); ++n) {
            int cx = chunks.get(n)[0], cz = chunks.get(n)[1];
            o.i32(cx);
            o.i32(cz);
            o.u8(n < whole ? 1 : 0);
            make(maker, o, cx, cz, n < whole);
        }
        return o.save(file);
    }

    interface Writer {
        int write(Path file, WorldgenRef.World world) throws Exception;
    }

    static final java.util.Map<String, Writer> FILES = new java.util.LinkedHashMap<>();

    static {
        FILES.put("random", WorldgenGolden::random);
        FILES.put("noise", WorldgenGolden::noise);
        FILES.put("density", WorldgenGolden::density);
        FILES.put("climate", WorldgenGolden::climate);
        FILES.put("terrain", WorldgenGolden::terrain);
    }
}
