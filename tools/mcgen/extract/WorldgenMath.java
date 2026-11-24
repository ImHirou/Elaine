// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen (math): where java.lang.Math's sine and cosine are not the sine and cosine
// src/worldgen computes for itself.
//
// Math.sin and Math.cos are HotSpot's own routines, and a feature that turns a random angle into
// blocks goes by their last bit. src/worldgen/jmath.cpp has a sine and a cosine of its own, made of
// nothing but IEEE additions and multiplications in a fixed order - and this file has the same
// ones, operation for operation (sinOwn, cosOwn: keep the two alike). For every argument a feature
// can have when its angle comes from a float - 2^24 of them a formula - and for the angles that are
// the same in every world (the search for where a world begins), the JVM is asked, and where it
// does not answer what sinOwn does, the difference is written down: math.bin, which
// gen_features.py hands on as worldgen_math.bin.
//
//   "WGMATH01", u32 sines, u32 cosines; then for the sine and for the cosine: the arguments (a
//   double's bits, ascending) and, a byte each, what is added to the bits of our own answer to
//   make the JVM's; each list padded to eight bytes. Little-endian.

import com.google.gson.JsonObject;
import java.io.ByteArrayOutputStream;
import java.nio.file.Files;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeMap;
import net.minecraft.SharedConstants;
import net.minecraft.core.RegistryAccess;

final class WorldgenMath {
    private WorldgenMath() {}

    // ------------------------------------------------------------------------------- our own

    private static final double PIO2_HI = 0x1.921fb54442d18p+0, PIO2_LO = 0x1.1a62633145c07p-54, PIO2_LO2 = -0x1.f1976b7ed8fbcp-110;
    private static final double TWO_OVER_PI = 0x1.45f306dc9c883p-1;
    private static final double[] S3 = {-0x1.5555555555555p-3, -0x1.5555555555555p-57}, S5 = {0x1.1111111111111p-7, 0x1.1111111111111p-63},
        S7 = {-0x1.a01a01a01a01ap-13, -0x1.a01a01a01a01ap-73}, S9 = {0x1.71de3a556c734p-19, -0x1.c154f8ddc6c00p-73};
    private static final double[] SIN_TAIL = {-0x1.ae64567f544e4p-26, 0x1.6124613a86d09p-33, -0x1.ae7f3e733b81fp-41, 0x1.952c77030ad4ap-49,
        -0x1.2f49b46814157p-57, 0x1.71b8ef6dcf572p-66, -0x1.761b41316381ap-75, 0x1.3f3ccdd165fa9p-84, -0x1.d1ab1c2dccea3p-94};
    private static final double[] C2 = {-0x1.0000000000000p-1, 0.0}, C4 = {0x1.5555555555555p-5, 0x1.5555555555555p-59},
        C6 = {-0x1.6c16c16c16c17p-10, 0x1.f49f49f49f49fp-65}, C8 = {0x1.a01a01a01a01ap-16, 0x1.a01a01a01a01ap-76},
        C10 = {-0x1.27e4fb7789f5cp-22, -0x1.cbbc05b4fa99ap-76};
    private static final double[] COS_TAIL = {0x1.1eed8eff8d898p-29, -0x1.93974a8c07c9dp-37, 0x1.ae7f3e733b81fp-45, -0x1.6827863b97d97p-53,
        0x1.e542ba4020225p-62, -0x1.0ce396db7f853p-70, 0x1.f2cf01972f578p-80, -0x1.88e85fc6a4e5ap-89, 0x1.0a18a2635085dp-98};

    /** A number as the sum of two doubles: {hi, lo}. */
    private static double[] twoSum(double a, double b) {
        double s = a + b;
        double bb = s - a;
        return new double[] {s, (a - (s - bb)) + (b - bb)};
    }

    private static double[] quickTwoSum(double a, double b) {
        double s = a + b;
        return new double[] {s, b - (s - a)};
    }

    private static double[] twoProd(double a, double b) {
        double p = a * b;
        double t = 134217729.0 * a;
        double ah = t - (t - a), al = a - ah;
        t = 134217729.0 * b;
        double bh = t - (t - b), bl = b - bh;
        return new double[] {p, ((ah * bh - p) + ah * bl + al * bh) + al * bl};
    }

    private static double[] add(double[] x, double[] y) {
        double[] s = twoSum(x[0], y[0]);
        return quickTwoSum(s[0], s[1] + (x[1] + y[1]));
    }

    private static double[] mul(double[] x, double[] y) {
        double[] p = twoProd(x[0], y[0]);
        return quickTwoSum(p[0], p[1] + (x[0] * y[1] + x[1] * y[0]));
    }

    /** x less the nearest multiple of pi / 2: {hi, lo, which multiple}. */
    private static double[] reduce(double x) {
        int n = (int) (x * TWO_OVER_PI + 0.5);
        if (n == 0) return new double[] {x, 0.0, 0.0};
        double m = (double) n;
        double[] t = twoProd(m, PIO2_HI);
        double[] d = twoSum(x, -t[0]);
        double low = ((d[1] - t[1]) - m * PIO2_LO) - m * PIO2_LO2;
        double[] r = twoSum(d[0], low);
        return new double[] {r[0], r[1], m};
    }

    private static double sinKernel(double[] r) {
        double[] r2 = mul(r, r);
        double z = r2[0];
        double tail = SIN_TAIL[8];
        for (int i = 7; i >= 0; --i) tail = tail * z + SIN_TAIL[i];
        double[] p = {tail, 0.0};
        p = add(mul(p, r2), S9);
        p = add(mul(p, r2), S7);
        p = add(mul(p, r2), S5);
        p = add(mul(p, r2), S3);
        p = mul(p, r2);
        p = add(mul(p, r), r);
        return p[0];
    }

    private static double cosKernel(double[] r) {
        double[] r2 = mul(r, r);
        double z = r2[0];
        double tail = COS_TAIL[8];
        for (int i = 7; i >= 0; --i) tail = tail * z + COS_TAIL[i];
        double[] p = {tail, 0.0};
        p = add(mul(p, r2), C10);
        p = add(mul(p, r2), C8);
        p = add(mul(p, r2), C6);
        p = add(mul(p, r2), C4);
        p = add(mul(p, r2), C2);
        p = mul(p, r2);
        p = add(p, new double[] {1.0, 0.0});
        return p[0];
    }

    /** jmath::sin_own. For 0 <= x < 8. */
    static double sinOwn(double x) {
        double[] reduced = reduce(x);
        double[] r = {reduced[0], reduced[1]};
        return switch ((int) reduced[2] & 3) {
            case 0 -> sinKernel(r);
            case 1 -> cosKernel(r);
            case 2 -> -sinKernel(r);
            default -> -cosKernel(r);
        };
    }

    /** jmath::cos_own. */
    static double cosOwn(double x) {
        double[] reduced = reduce(x);
        double[] r = {reduced[0], reduced[1]};
        return switch ((int) reduced[2] & 3) {
            case 0 -> cosKernel(r);
            case 1 -> -sinKernel(r);
            case 2 -> -cosKernel(r);
            default -> sinKernel(r);
        };
    }

    // ------------------------------------------------------------------------------- what is asked

    /** How many angles a float gives: RandomSource.nextFloat is one of 2^24 numbers. */
    static final int FLOATS = 1 << 24;

    /** OreFeature.place: `random.nextFloat() * (float) Math.PI`, then Math.sin and Math.cos of it as a double. */
    static double oreAngle(int k) {
        float dir = (k * 5.9604645E-8F) * 3.1415927f;
        return (double) dir;
    }

    /** FancyTrunkPlacer.placeTrunk: the angle a limb leaves the trunk at - twice a random float, as a double times pi. */
    static double fancyTrunkAngle(int k) {
        return (double) ((k * 5.9604645E-8F) * 2.0F) * 3.141592653589793;
    }

    /**
     * NoiseSpawnFinder.radialSearch: the angles of its rings, which are the same for every world -
     * (2048, 512) first, then (512, 32), as its constructor has them.
     */
    static double[] spawnSearchAngles() {
        List<Double> angles = new ArrayList<>();
        for (float[] search : new float[][] {{2048.0f, 512.0f}, {512.0f, 32.0f}}) {
            float maxRadius = search[0], radiusIncrement = search[1];
            float angle = 0.0f;
            float radius = radiusIncrement;
            while (radius <= maxRadius) {
                angles.add((double) angle);
                angle = angle + (radiusIncrement / radius);
                if ((double) angle > 6.283185307179586) {
                    angle = 0.0f;
                    radius = radius + radiusIncrement;
                }
            }
        }
        return angles.stream().mapToDouble(Double::doubleValue).toArray();
    }

    /** Arguments Math.sin and Math.cos are asked about: so many, the k-th of them. */
    record Domain(String name, int count, java.util.function.IntToDoubleFunction of) {}

    /**
     * Every argument src/worldgen takes the sine and cosine of where vanilla does: the formulas an
     * angle is made by of a float, and the angles that are the same for every world. The first is
     * the one whose answers the golden script folds into one number; the others it writes out.
     */
    static Domain[] domains() {
        double[] spawn = spawnSearchAngles();
        return new Domain[] {new Domain("ore", FLOATS, WorldgenMath::oreAngle), new Domain("spawn search", spawn.length, k -> spawn[k]),
                             new Domain("fancy trunk", FLOATS, WorldgenMath::fancyTrunkAngle)};
    }

    private static void list(ByteArrayOutputStream out, TreeMap<Long, Byte> corrections) {
        for (long key : corrections.keySet())
            for (int i = 0; i < 8; ++i) out.write((int) (key >>> (8 * i)));
        for (byte delta : corrections.values()) out.write(delta);
        for (int i = corrections.size(); (i & 7) != 0; ++i) out.write(0);
    }

    static JsonObject math(RegistryAccess registries) throws Exception {
        // (Arguments are not negative: as longs their bits are in the order of the numbers.)
        TreeMap<Long, Byte> sines = new TreeMap<>(), cosines = new TreeMap<>();
        TreeMap<Long, Integer> sizes = new TreeMap<>();
        long asked = 0;
        for (Domain domain : domains())
            for (int k = 0; k < domain.count(); ++k) {
                double x = domain.of().applyAsDouble(k);
                ++asked;
                if (!(x >= 0.0 && x < 8.0)) {
                    WorldgenRef.problem("an argument of " + x + " (" + domain.name() + "): src/worldgen's sine is written for 0 .. 8");
                    return new JsonObject();
                }
                long ds = Double.doubleToRawLongBits(Math.sin(x)) - Double.doubleToRawLongBits(sinOwn(x));
                long dc = Double.doubleToRawLongBits(Math.cos(x)) - Double.doubleToRawLongBits(cosOwn(x));
                if (ds < -100 || ds > 100 || dc < -100 || dc > 100) {
                    WorldgenRef.problem("Math.sin or cos of " + x + " is not near what src/worldgen computes: the two have come apart");
                    return new JsonObject();
                }
                if (ds != 0 && sines.put(Double.doubleToRawLongBits(x), (byte) ds) == null) sizes.merge(ds, 1, Integer::sum);
                if (dc != 0 && cosines.put(Double.doubleToRawLongBits(x), (byte) dc) == null) sizes.merge(dc, 1, Integer::sum);
            }
        ByteArrayOutputStream out = new ByteArrayOutputStream();
        out.writeBytes("WGMATH01".getBytes(java.nio.charset.StandardCharsets.US_ASCII));
        for (int count : new int[] {sines.size(), cosines.size()})
            for (int i = 0; i < 4; ++i) out.write(count >>> (8 * i));
        list(out, sines);
        list(out, cosines);
        Files.write(WorldgenRef.out.resolve("math.bin"), out.toByteArray());
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.addProperty("java", System.getProperty("java.vm.name") + " " + System.getProperty("java.vm.version"));
        root.addProperty("asked", asked);
        root.addProperty("sines", sines.size());
        root.addProperty("cosines", cosines.size());
        JsonObject by = new JsonObject();
        for (var e : sizes.entrySet()) by.addProperty(Long.toString(e.getKey()), e.getValue());
        root.add("differences_in_the_last_place", by);
        root.addProperty("bytes", out.size());
        return root;
    }

    /**
     * For the golden script: what the JVM answers over the first domain, as one number a function,
     * and a few of the answers themselves; then every argument of the others with its answers.
     */
    static void golden(WorldgenGolden.Out o) {
        Domain[] domains = domains();
        long sines = 0xcbf29ce484222325L, cosines = 0xcbf29ce484222325L;
        List<long[]> samples = new ArrayList<>();
        for (int k = 0; k < domains[0].count(); ++k) {
            double x = domains[0].of().applyAsDouble(k);
            long s = Double.doubleToRawLongBits(Math.sin(x)), c = Double.doubleToRawLongBits(Math.cos(x));
            sines = WorldgenGolden.fnv(WorldgenGolden.fnv(sines, (int) s), (int) (s >>> 32));
            cosines = WorldgenGolden.fnv(WorldgenGolden.fnv(cosines, (int) c), (int) (c >>> 32));
            if (k % 1048573 == 0) samples.add(new long[] {k, Double.doubleToRawLongBits(x), s, c});
        }
        o.i64(sines);
        o.i64(cosines);
        o.i32(samples.size());
        for (long[] sample : samples) {
            o.i32((int) sample[0]);
            for (int i = 1; i < 4; ++i) o.i64(sample[i]);
        }
        o.i32(domains.length - 1);
        for (int d = 1; d < domains.length; ++d) {
            o.str(domains[d].name());
            o.i32(domains[d].count());
            if (domains[d].count() > 4096) {
                // Too many to write out: folded into a number a function, as the first domain is (the test has the formula).
                long sineFold = 0xcbf29ce484222325L, cosineFold = 0xcbf29ce484222325L;
                for (int k = 0; k < domains[d].count(); ++k) {
                    double x = domains[d].of().applyAsDouble(k);
                    long sine = Double.doubleToRawLongBits(Math.sin(x)), cosine = Double.doubleToRawLongBits(Math.cos(x));
                    sineFold = WorldgenGolden.fnv(WorldgenGolden.fnv(sineFold, (int) sine), (int) (sine >>> 32));
                    cosineFold = WorldgenGolden.fnv(WorldgenGolden.fnv(cosineFold, (int) cosine), (int) (cosine >>> 32));
                }
                o.i64(sineFold);
                o.i64(cosineFold);
                continue;
            }
            for (int k = 0; k < domains[d].count(); ++k) {
                double x = domains[d].of().applyAsDouble(k);
                o.i64(Double.doubleToRawLongBits(x));
                o.i64(Double.doubleToRawLongBits(Math.sin(x)));
                o.i64(Double.doubleToRawLongBits(Math.cos(x)));
            }
        }
    }
}
