// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen: what vanilla's world generation is made of once the game has loaded, and what
// it computes - the reference the C++ port (src/worldgen) is generated from and held to.
//
//   program   the overworld's density functions as vanilla *compiled* them (RandomState.getSampler:
//             references inlined, caches shared, uniform axes sliced, constants folded into the
//             samplers) - one graph of sampler objects, dumped field by field. A compiled sampler
//             is what decides the bits: `x / c` became `x * (1/c)`, a `min` whose ranges do not
//             overlap is gone. The port runs this graph, not the JSON it came from.
//   climate   the overworld's biome parameter list and the R-tree vanilla searches it with, node
//             by node in the order the search walks them (a tie goes to whoever comes first).
//   golden    what vanilla computes, as scripts the unit tests replay (WorldgenGolden.java): only
//             when it is asked for - the tables are not made of it.
//
// A graph is a list of nodes; a node is {"c": class, field: value...} - and "@name" where the
// object has a name of its own (a noise's) -; a value is a number, a string, a list, or
// {"#": index of another node}. A float is written as the double it widens
// to, which names it exactly. "schemas" gives the declared type of every field of every class.
//
// It loads the game as GameData does: no world, no server, no network.
// Run through `tools/mcgen/mcgen.py worldgen`.

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.JsonPrimitive;
import com.mojang.datafixers.util.Pair;
import java.io.PrintStream;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.lang.reflect.RecordComponent;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Collection;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.Set;
import net.minecraft.SharedConstants;
import net.minecraft.core.Holder;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.world.level.biome.Climate;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterList;
import net.minecraft.world.level.biome.MultiNoiseBiomeSourceParameterLists;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.levelgen.NoiseGeneratorSettings;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.densityfunction.DensityFunction;
import net.minecraft.world.level.levelgen.densityfunction.DensitySampler;
import net.minecraft.world.level.levelgen.synth.GradientNoise;
import net.minecraft.world.level.levelgen.synth.Noise;

public final class WorldgenRef {
    static final List<String> problems = new ArrayList<>();

    static void problem(String what) {
        problems.add(what);
        System.err.println("worldgen: PROBLEM: " + what);
    }

    /** The seeds the program is compiled for: it must come out the same but for what a seed draws. */
    static final long SEED_A = 20261123L, SEED_B = -4172144997902289642L;

    // ------------------------------------------------------------------------------- the dumper

    /** An object graph as nodes, by identity: what is reached twice is one node. */
    static final class Graph {
        final IdentityHashMap<Object, Integer> ids = new IdentityHashMap<>();
        final JsonArray nodes = new JsonArray();
        final JsonObject schemas = new JsonObject();
        /** Objects that have a name of their own (a noise by its registry key). */
        final IdentityHashMap<Object, String> names = new IdentityHashMap<>();

        static JsonElement real(double v) {
            // JSON has no NaN and no infinities; the port needs them exactly.
            if (Double.isNaN(v)) return new JsonPrimitive("NaN");
            if (Double.isInfinite(v)) return new JsonPrimitive(v > 0 ? "Infinity" : "-Infinity");
            return new JsonPrimitive(v);
        }

        JsonElement value(Object v) {
            if (v == null) return JsonNull.INSTANCE;
            if (v instanceof Boolean b) return new JsonPrimitive(b);
            if (v instanceof Float f) return real(f.doubleValue());
            if (v instanceof Double d) return real(d);
            if (v instanceof Number n) return new JsonPrimitive(n);
            if (v instanceof Character c) return new JsonPrimitive(String.valueOf(c));
            if (v instanceof String s) return new JsonPrimitive(s);
            if (v instanceof Enum<?> e) return new JsonPrimitive(e.name());
            if (v instanceof Identifier id) return new JsonPrimitive(id.toString());
            if (v instanceof ResourceKey<?> key) return new JsonPrimitive(key.identifier().toString());
            if (v instanceof Optional<?> o) return value(o.orElse(null));
            if (v instanceof BlockState s) return tagged("state", new JsonPrimitive(Block.getId(s)));
            if (v instanceof Holder<?> h) {
                Optional<? extends ResourceKey<?>> key = h.unwrapKey();
                return key.isPresent() ? tagged("holder", new JsonPrimitive(key.get().identifier().toString())) : value(h.value());
            }
            if (v instanceof Pair<?, ?> p) {
                JsonArray a = new JsonArray();
                a.add(value(p.getFirst()));
                a.add(value(p.getSecond()));
                return a;
            }
            if (v.getClass().isArray()) {
                JsonArray a = new JsonArray();
                for (int i = 0, n = Array.getLength(v); i < n; ++i) a.add(value(Array.get(v, i)));
                return a;
            }
            if (v instanceof Collection<?> c) {
                JsonArray a = new JsonArray();
                for (Object e : c) a.add(value(e));
                return a;
            }
            if (v instanceof Map<?, ?> m) {
                JsonArray a = new JsonArray();
                for (Map.Entry<?, ?> e : m.entrySet()) {
                    JsonArray pair = new JsonArray();
                    pair.add(value(e.getKey()));
                    pair.add(value(e.getValue()));
                    a.add(pair);
                }
                return tagged("map", a);
            }
            if (!Dump.vanilla(v.getClass()) && !lambda(v.getClass())) return tagged("opaque", new JsonPrimitive(v.getClass().getName()));
            if (OPAQUE.contains(v.getClass().getName())) return tagged("opaque", new JsonPrimitive(Dump.simple(v.getClass())));
            return node(v);
        }

        static boolean lambda(Class<?> k) {
            return k.isHidden() || k.getName().contains("$$Lambda");
        }

        static JsonObject tagged(String tag, JsonElement v) {
            JsonObject o = new JsonObject();
            o.add(tag, v);
            return o;
        }

        JsonElement node(Object v) {
            Integer id = ids.get(v);
            if (id == null) {
                id = nodes.size();
                ids.put(v, id);
                JsonObject n = new JsonObject();
                nodes.add(n);  // in place before its fields: a cycle finds it
                Class<?> k = v.getClass();
                String name = lambda(k) ? "lambda:" + k.getName().replaceAll("\\$\\$Lambda.*", "").replaceAll("^.*\\.", "") : Dump.simple(k);
                n.addProperty("c", name);
                if (names.containsKey(v)) n.addProperty("@name", names.get(v));
                JsonObject schema = schemas.has(name) ? null : new JsonObject();
                for (Field f : fields(k)) {
                    Object fv;
                    try {
                        fv = f.get(v);
                    } catch (IllegalAccessException e) {
                        problem(name + "." + f.getName() + ": " + e);
                        continue;
                    }
                    n.add(f.getName(), value(fv));
                    if (schema != null) schema.addProperty(f.getName(), f.getType().getSimpleName());
                }
                if (schema != null) schemas.add(name, schema);
            }
            return tagged("#", new JsonPrimitive(id));
        }

        /** Instance fields, the class's own after its superclasses': what the object is made of. */
        static List<Field> fields(Class<?> k) {
            List<Class<?>> chain = new ArrayList<>();
            for (Class<?> c = k; c != null && c != Object.class && c != Record.class; c = c.getSuperclass()) chain.add(0, c);
            List<Field> out = new ArrayList<>();
            for (Class<?> c : chain)
                for (Field f : c.getDeclaredFields()) {
                    if (Modifier.isStatic(f.getModifiers())) continue;
                    try {
                        f.setAccessible(true);
                    } catch (RuntimeException e) {
                        problem(c.getName() + "." + f.getName() + " cannot be read: " + e);
                        continue;
                    }
                    out.add(f);
                }
            return out;
        }
    }

    /** What a graph stops at: the machinery around the samplers, not a part of them. */
    static final Set<String> OPAQUE = Set.of(
        "net.minecraft.world.level.levelgen.RandomState",
        "net.minecraft.world.level.levelgen.densityfunction.DensityFunctionCompiler",
        "net.minecraft.world.level.levelgen.material.MaterialSystem");

    // ------------------------------------------------------------------------------- program

    /** Every density function vanilla asks a sampler of, by where it is named. */
    static Map<String, DensityFunction> roots(NoiseGeneratorSettings settings) throws Exception {
        Map<String, DensityFunction> out = new LinkedHashMap<>();
        components("router", settings.noiseRouter(), out);
        if (settings.aquifers().isPresent()) components("aquifer", settings.aquifers().get(), out);
        int point = 0;
        for (var target : settings.spawnTarget()) {
            for (var e : target.parameters().entrySet())
                out.put("spawn/" + point + "/" + e.getKey().unwrapKey().orElseThrow().identifier(), e.getKey().value());
            ++point;
        }
        // The material rules name density functions too (ore veins): found wherever they are.
        find("material", settings.materialRule(), out, new IdentityHashMap<>());
        return out;
    }

    static void components(String prefix, Record r, Map<String, DensityFunction> out) throws Exception {
        for (RecordComponent c : r.getClass().getRecordComponents()) {
            if (!DensityFunction.class.isAssignableFrom(c.getType())) continue;
            Field f = r.getClass().getDeclaredField(c.getName());
            f.setAccessible(true);
            out.put(prefix + "/" + c.getName(), (DensityFunction) f.get(r));
        }
    }

    /** Density functions under `v`, each named by the registry entries and fields on the way to it. */
    static void find(String path, Object v, Map<String, DensityFunction> out, IdentityHashMap<Object, Boolean> seen) throws Exception {
        if (v == null || v instanceof String || v instanceof Number || v instanceof Boolean || v instanceof Enum<?> || v instanceof BlockState) return;
        if (seen.put(v, true) != null) return;
        if (v instanceof DensityFunction fn) {
            String name = path;
            for (int n = 2; out.containsKey(name); ++n) name = path + "#" + n;
            out.put(name, fn);
            return;
        }
        if (v instanceof Holder<?> h) {
            Optional<? extends ResourceKey<?>> key = h.unwrapKey();
            if (key.isPresent() && !key.get().registry().getPath().startsWith("worldgen/material")) return;
            find(key.isPresent() ? "material/" + key.get().identifier().getPath() : path, h.value(), out, seen);
            return;
        }
        if (v instanceof Optional<?> o) {
            find(path, o.orElse(null), out, seen);
            return;
        }
        if (v instanceof Collection<?> c) {
            for (Object e : c) find(path, e, out, seen);
            return;
        }
        if (v instanceof Map<?, ?> m) {
            for (Object e : m.values()) find(path, e, out, seen);
            return;
        }
        if (!Dump.vanilla(v.getClass())) return;
        for (Field f : Graph.fields(v.getClass())) find(path + "." + f.getName(), f.get(v), out, seen);
    }

    /** A (private) field of `o`, wherever in its classes it is declared. */
    static Object field(Object o, String name) throws Exception {
        for (Class<?> k = o.getClass(); k != null; k = k.getSuperclass())
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f.get(o);
            } catch (NoSuchFieldException e) {
                // in a superclass, then
            }
        throw new NoSuchFieldException(o.getClass().getName() + "." + name);
    }

    interface Visitor {
        void visit(Object o) throws Exception;
    }

    /** Every vanilla object under `v`, once each. */
    static void walk(Object v, IdentityHashMap<Object, Boolean> seen, Visitor visitor) throws Exception {
        if (v == null || v instanceof String || v instanceof Number || v instanceof Boolean || v instanceof Enum<?>) return;
        if (seen.put(v, true) != null) return;
        if (v.getClass().isArray()) {
            if (!v.getClass().getComponentType().isPrimitive())
                for (int i = 0, n = Array.getLength(v); i < n; ++i) walk(Array.get(v, i), seen, visitor);
            return;
        }
        if (v instanceof Collection<?> c) {
            for (Object e : c) walk(e, seen, visitor);
            return;
        }
        if (!Dump.vanilla(v.getClass()) || OPAQUE.contains(v.getClass().getName()) || v instanceof Holder<?>) return;
        visitor.visit(v);
        for (Field f : Graph.fields(v.getClass())) walk(f.get(v), seen, visitor);
    }

    /** The overworld of one seed: its random state, what it compiled, and its noises by name. */
    record World(RegistryAccess registries, long seed, NoiseGeneratorSettings settings, RandomState state, Map<String, DensitySampler> samplers,
                 Map<String, Noise> noises) {}

    static World world(RegistryAccess registries, long seed) throws Exception {
        NoiseGeneratorSettings settings = registries.lookupOrThrow(Registries.NOISE_SETTINGS).getOrThrow(NoiseGeneratorSettings.OVERWORLD).value();
        var registry = registries.lookupOrThrow(Registries.NOISE);
        RandomState state = RandomState.create(registry, seed, settings);
        Map<String, DensitySampler> samplers = new LinkedHashMap<>();
        for (Map.Entry<String, DensityFunction> e : roots(settings).entrySet()) samplers.put(e.getKey(), state.getSampler(e.getValue()));
        // A noise is made once per RandomState and shared by whatever names it: every noise of
        // the registry, whether a density function asked for it or not (the nether's two are
        // made another way, by the nether's own generator).
        Map<String, Noise> noises = new java.util.TreeMap<>();
        for (var key : registry.listElementIds().toList()) {
            if (key.identifier().getPath().startsWith("nether/")) continue;
            noises.put(key.identifier().toString(), state.getOrCreateNoise(key));
        }
        // old_blended_noise makes three stacks of its own, from the seed of "minecraft:terrain":
        // Lerp(Clamp(ConstAdd(main)), minLimit, maxLimit) - BlendedNoise.compileSampler.
        IdentityHashMap<Object, Boolean> seen = new IdentityHashMap<>();
        for (DensitySampler sampler : samplers.values())
            walk(sampler, seen, o -> {
                if (!o.getClass().getName().endsWith("LerpFunction$Sampler")) return;
                Object first = field(o, "first"), second = field(o, "second");
                if (!first.getClass().getName().endsWith("NoiseFunction$Sampler")) return;
                Object min = field(first, "noise"), max = field(second, "noise");
                if (!min.getClass().getName().endsWith("NoiseStack$SmearedPerlin")) return;
                Object main = field(field(field(field(o, "alpha"), "input"), "left"), "noise");
                noises.put("blended:min_limit", (Noise) min);
                noises.put("blended:max_limit", (Noise) max);
                noises.put("blended:main", (Noise) main);
            });
        if (!noises.containsKey("blended:main")) problem("old_blended_noise is not compiled into Lerp(Clamp(ConstAdd(main)), min, max) any more");
        return new World(registries, seed, settings, state, samplers, noises);
    }

    record Program(Graph graph, JsonObject roots, JsonObject noises) {}

    static Program dump(World w) {
        Graph g = new Graph();
        for (Map.Entry<String, Noise> e : w.noises().entrySet()) g.names.put(e.getValue(), e.getKey());
        JsonObject roots = new JsonObject();
        for (Map.Entry<String, DensitySampler> e : w.samplers().entrySet()) roots.add(e.getKey(), g.value(e.getValue()));
        JsonObject noises = new JsonObject();
        for (Map.Entry<String, Noise> e : w.noises().entrySet()) noises.add(e.getKey(), g.value(e.getValue()));
        return new Program(g, roots, noises);
    }

    /** What a seed draws: a noise's permutation and offsets. Everything else must not depend on it. */
    static boolean seeded(JsonObject node, String field, Class<?> k) {
        return k != null && GradientNoise.class.isAssignableFrom(k) && (field.equals("perms") || field.startsWith("offset"));
    }

    static void sameProgram(Program a, Program b, Map<String, Class<?>> classes) {
        if (!a.roots().equals(b.roots()) || !a.noises().equals(b.noises()) || a.graph().nodes.size() != b.graph().nodes.size()) {
            problem("the compiled program has another shape for another seed");
            return;
        }
        for (int i = 0; i < a.graph().nodes.size(); ++i) {
            JsonObject x = a.graph().nodes.get(i).getAsJsonObject(), y = b.graph().nodes.get(i).getAsJsonObject();
            if (!x.keySet().equals(y.keySet())) {
                problem("node " + i + " has other fields for another seed");
                return;
            }
            Class<?> k = classes.get(x.get("c").getAsString());
            for (String field : x.keySet())
                if (!seeded(x, field, k) && !x.get(field).equals(y.get(field))) {
                    problem("node " + i + " (" + x.get("c").getAsString() + ")." + field + " depends on the seed: " + x.get(field) + " / " + y.get(field));
                    return;
                }
        }
    }

    static JsonObject program(RegistryAccess registries) throws Exception {
        Program a = dump(world(registries, SEED_A)), b = dump(world(registries, SEED_B));
        Map<String, Class<?>> classes = new LinkedHashMap<>();
        for (Object o : a.graph().ids.keySet()) classes.put(a.graph().nodes.get(a.graph().ids.get(o)).getAsJsonObject().get("c").getAsString(), o.getClass());
        sameProgram(a, b, classes);
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.addProperty("settings", NoiseGeneratorSettings.OVERWORLD.identifier().toString());
        root.addProperty("seed", SEED_A);
        root.add("roots", a.roots());
        root.add("noises", a.noises());
        root.add("schemas", a.graph().schemas);
        root.add("nodes", a.graph().nodes);
        return root;
    }

    // ------------------------------------------------------------------------------- climate

    static JsonObject climate(RegistryAccess registries) throws Exception {
        MultiNoiseBiomeSourceParameterList list =
            registries.lookupOrThrow(Registries.MULTI_NOISE_BIOME_SOURCE_PARAMETER_LIST).getOrThrow(MultiNoiseBiomeSourceParameterLists.OVERWORLD).value();
        Climate.ParameterList<?> parameters = list.parameters();
        Graph g = new Graph();
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.add("list", g.value(parameters));
        root.add("schemas", g.schemas);
        root.add("nodes", g.nodes);
        return root;
    }

    // ------------------------------------------------------------------------------- main

    interface Section {
        JsonObject run(RegistryAccess registries) throws Exception;
    }

    static final Map<String, Section> SECTIONS = new LinkedHashMap<>();
    /** What a run without arguments writes: what `generate` reads. */
    static final List<String> TABLES = List.of("program", "climate");
    static Path out;

    static {
        SECTIONS.put("program", WorldgenRef::program);
        SECTIONS.put("climate", WorldgenRef::climate);
        SECTIONS.put("golden", WorldgenRef::golden);
        SECTIONS.put("census", WorldgenFeatures::census);
        SECTIONS.put("replay", WorldgenFeatures::replay);
    }

    static JsonObject golden(RegistryAccess registries) throws Exception {
        Path dir = out.resolve("golden");
        Files.createDirectories(dir);
        JsonObject files = new JsonObject();
        World w = world(registries, SEED_A);
        for (Map.Entry<String, WorldgenGolden.Writer> e : WorldgenGolden.FILES.entrySet())
            files.addProperty(e.getKey() + ".bin", e.getValue().write(dir.resolve(e.getKey() + ".bin"), w));
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.add("files", files);
        return root;
    }

    public static void main(String[] args) throws Exception {
        final PrintStream stdout = System.out;  // Bootstrap redirects System.out into the game log
        out = Path.of(args[0]);
        List<String> wanted = args.length > 1 ? List.of(args).subList(1, args.length) : TABLES;
        for (String name : wanted)
            if (!SECTIONS.containsKey(name)) {
                System.err.println("worldgen: no section '" + name + "' (have: " + String.join(", ", SECTIONS.keySet()) + ")");
                System.exit(2);
            }
        // (a feature reads structure templates out of the game's resources: those sections keep them open)
        GameData.Env env = wanted.stream().anyMatch(WorldgenFeatures.SECTIONS::contains) ? WorldgenFeatures.load() : GameData.load();
        Dump.index(env.registries());
        Files.createDirectories(out);
        for (String name : wanted) {
            long t0 = System.nanoTime();
            JsonObject root;
            try {
                root = SECTIONS.get(name).run(env.registries());
            } catch (Exception | LinkageError e) {
                e.printStackTrace();
                problem(name + ": " + e);
                continue;
            }
            Dump.write(out.resolve(name + ".json"), root);
            StringBuilder line = new StringBuilder("worldgen: " + name + ":");
            for (Map.Entry<String, JsonElement> e : root.entrySet())
                if (e.getValue().isJsonArray()) line.append(' ').append(e.getValue().getAsJsonArray().size()).append(' ').append(e.getKey()).append(',');
                else if (e.getValue().isJsonObject()) line.append(' ').append(e.getValue().getAsJsonObject().size()).append(' ').append(e.getKey()).append(',');
            stdout.println(line + " " + (System.nanoTime() - t0) / 1_000_000 + " ms");
        }
        if (!problems.isEmpty()) {
            stdout.println("worldgen: " + problems.size() + " problems (see above): vanilla has changed under the extractor");
            System.exit(1);
        }
        System.exit(0);  // worker pools of the game are not daemon threads
    }
}
