// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen gamedata: the generic half - turn any vanilla object into JSON without knowing its class.
//
// Rules, in order (enc): plain values as themselves; anything that lives in a static registry as its
// name; block and fluid states as their ids; containers element by element (maps and sets sorted, so
// two runs give the same bytes); a class with a CODEC through that codec (vanilla's own JSON form);
// any other vanilla object field by field. Lambdas and codecs are opaque and left out.
//
// Field names are vanilla's own (the jars are unobfuscated since 26.1): a consumer that needs one
// must fail loudly when it is gone, the same way gen.py does for the static initializers it parses.

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonNull;
import com.google.gson.JsonObject;
import com.google.gson.JsonPrimitive;
import com.mojang.datafixers.util.Either;
import com.mojang.datafixers.util.Pair;
import com.mojang.serialization.Codec;
import com.mojang.serialization.DataResult;
import com.mojang.serialization.DynamicOps;
import com.mojang.serialization.JsonOps;
import java.io.IOException;
import java.io.Writer;
import java.lang.reflect.Array;
import java.lang.reflect.Field;
import java.lang.reflect.Method;
import java.lang.reflect.Modifier;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collection;
import java.util.Collections;
import java.util.EnumMap;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.IdentityHashMap;
import java.util.LinkedHashMap;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import java.util.OptionalDouble;
import java.util.OptionalInt;
import java.util.OptionalLong;
import java.util.Set;
import java.util.SortedMap;
import java.util.SortedSet;
import java.util.TreeSet;
import java.util.UUID;
import net.minecraft.core.Holder;
import net.minecraft.core.HolderSet;
import net.minecraft.core.IdMap;
import net.minecraft.core.Registry;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.Vec3i;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.chat.Component;
import net.minecraft.network.chat.ComponentSerialization;
import net.minecraft.resources.Identifier;
import net.minecraft.resources.ResourceKey;
import net.minecraft.tags.TagKey;
import net.minecraft.world.flag.FeatureFlagSet;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.item.ItemStack;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.VoxelShape;

final class Dump {
    private Dump() {}

    /** Everything that went wrong without stopping the run; a non-empty list fails the command. */
    static final List<String> problems = new ArrayList<>();

    static void problem(String what) {
        if (problems.size() < 200) problems.add(what);
    }

    static RegistryAccess registries;
    static DynamicOps<JsonElement> ops = JsonOps.INSTANCE;

    /** Every entry of every static registry, by identity. */
    private static final IdentityHashMap<Object, String> ENTRY_NAMES = new IdentityHashMap<>();

    static void index(RegistryAccess access) {
        registries = access;
        ops = access.createSerializationContext(JsonOps.INSTANCE);
        for (Registry<?> r : BuiltInRegistries.REGISTRY) indexOne(r);
    }

    private static <T> void indexOne(Registry<T> r) {
        for (T e : r) {
            // Some registries hold plain values (custom_stat: Identifier, *_type: codecs). Those are
            // not "an entry" when met elsewhere, and a codec may be registered under several names.
            if (e instanceof Identifier || !e.getClass().getName().startsWith("net.minecraft.") || opaque(e)) continue;
            ENTRY_NAMES.putIfAbsent(e, String.valueOf(r.getKey(e)));
        }
    }

    static String nameOf(Object entry) {
        return ENTRY_NAMES.get(entry);
    }

    // ------------------------------------------------------------------------------------ names

    /** Class name without the package: `BlockBehaviour$Properties`. */
    static String simple(Class<?> k) {
        String n = k.getName();
        return n.substring(n.lastIndexOf('.') + 1);
    }

    /** Superclasses from `c` up to (not including) `stop` or the first non-vanilla class. */
    static JsonArray chain(Class<?> c, Class<?> stop) {
        JsonArray out = new JsonArray();
        for (Class<?> k = c; k != null && k != stop && k != Object.class && vanilla(k); k = k.getSuperclass()) out.add(simple(k));
        return out;
    }

    /** Every vanilla interface the class implements, directly or not, sorted. */
    static JsonArray interfaces(Class<?> c) {
        TreeSet<String> names = new TreeSet<>();
        collectInterfaces(c, names);
        JsonArray out = new JsonArray();
        for (String n : names) out.add(n);
        return out;
    }

    private static void collectInterfaces(Class<?> c, Set<String> into) {
        for (Class<?> k = c; k != null; k = k.getSuperclass())
            for (Class<?> i : k.getInterfaces()) {
                if (vanilla(i)) into.add(simple(i));
                collectInterfaces(i, into);
            }
    }

    static boolean vanilla(Class<?> k) {
        return k.getName().startsWith("net.minecraft.");
    }

    // ------------------------------------------------------------------------------------ hooks

    /** The overridable methods of some base classes: what a subclass can change about behaviour. */
    static final class Hooks {
        private final List<Method> methods = new ArrayList<>();
        private final Map<String, Integer> overloads = new HashMap<>();
        private final Set<Class<?>> bases;

        Hooks(Class<?>... baseClasses) {
            bases = Set.of(baseClasses);
            Set<String> seen = new LinkedHashSet<>();
            for (Class<?> b : baseClasses)
                for (Method m : sorted(b.getDeclaredMethods())) {
                    int mod = m.getModifiers();
                    if (Modifier.isStatic(mod) || Modifier.isPrivate(mod) || Modifier.isFinal(mod)) continue;
                    if (m.isSynthetic() || m.isBridge()) continue;
                    if (!seen.add(signature(m))) continue;  // Block overriding BlockBehaviour: one hook
                    methods.add(m);
                    overloads.merge(m.getName(), 1, Integer::sum);
                }
        }

        /** {hook: class that overrides it} for the hooks that `c` does not take from a base class. */
        JsonObject overridden(Class<?> c) {
            JsonObject out = new JsonObject();
            for (Method m : methods) {
                Class<?> by = null;
                for (Class<?> k = c; k != null && by == null; k = k.getSuperclass())
                    try {
                        Method d = k.getDeclaredMethod(m.getName(), m.getParameterTypes());
                        if (!d.isBridge() && !d.isSynthetic()) by = k;
                    } catch (NoSuchMethodException e) {
                        // not declared here, look further up
                    }
                if (by == null || bases.contains(by)) continue;
                out.addProperty(overloads.get(m.getName()) > 1 ? signature(m) : m.getName(), simple(by));
            }
            return out;
        }

        JsonArray names() {
            JsonArray out = new JsonArray();
            for (Method m : methods) out.add(overloads.get(m.getName()) > 1 ? signature(m) : m.getName());
            return out;
        }

        private static String signature(Method m) {
            StringBuilder sb = new StringBuilder(m.getName()).append('(');
            Class<?>[] p = m.getParameterTypes();
            for (int i = 0; i < p.length; ++i) sb.append(i == 0 ? "" : ",").append(simple(p[i]));
            return sb.append(')').toString();
        }

        private static Method[] sorted(Method[] ms) {
            Method[] out = ms.clone();  // getDeclaredMethods() has no defined order
            Arrays.sort(out, (a, b) -> signature(a).compareTo(signature(b)));
            return out;
        }
    }

    // ------------------------------------------------------------------------------------ reflection

    static Field field(Class<?> c, String name) {
        for (Class<?> k = c; k != null; k = k.getSuperclass())
            try {
                Field f = k.getDeclaredField(name);
                f.setAccessible(true);
                return f;
            } catch (NoSuchFieldException e) {
                // look further up
            }
        return null;
    }

    /** Value of a (private) field, or null with a problem noted: the field is how vanilla names it. */
    static Object get(Object target, Class<?> c, String name) {
        Field f = field(c, name);
        if (f == null) {
            problem("field " + c.getName() + "." + name + " is gone");
            return null;
        }
        try {
            return f.get(target);
        } catch (ReflectiveOperationException e) {
            problem("field " + c.getName() + "." + name + ": " + e);
            return null;
        }
    }

    /** Call a no-argument (protected) method found anywhere up the class chain. */
    static Object call(Object target, String name) {
        for (Class<?> k = target.getClass(); k != null; k = k.getSuperclass())
            try {
                Method m = k.getDeclaredMethod(name);
                m.setAccessible(true);
                return m.invoke(target);
            } catch (NoSuchMethodException e) {
                // look further up
            } catch (ReflectiveOperationException e) {
                problem("method " + k.getName() + "." + name + "(): " + e);
                return null;
            }
        problem("method " + target.getClass().getName() + "." + name + "() is gone");
        return null;
    }

    // ------------------------------------------------------------------------------------ encoder

    private static final int MAX_DEPTH = 7;
    private static final int MAX_NODES = 200_000;  // per top-level value: a runaway object graph stops here
    private static final Set<String> SKIP_FIELDS = Set.of("builtInRegistryHolder", "stateDefinition", "LOGGER");
    private static final Map<Class<?>, Optional<Codec<Object>>> CODECS = new HashMap<>();
    private static final IdentityHashMap<Object, Boolean> active = new IdentityHashMap<>();
    private static int nodes;

    static JsonElement enc(Object v) {
        nodes = 0;
        active.clear();
        return enc(v, 1);
    }

    /** The instance fields of `v`, declared in vanilla classes, as an object. */
    static JsonObject fields(Object v) {
        return fields(v, null);
    }

    /** The same, leaving out what `base` and the classes above it declare. */
    static JsonObject fields(Object v, Class<?> base) {
        nodes = 0;
        active.clear();
        JsonObject out = new JsonObject();
        active.put(v, true);
        fieldsInto(out, v, 1, base);
        return out;
    }

    /** Code (lambdas, codecs) and running state (random sources, counters): not data. */
    static boolean opaque(Object v) {
        Class<?> c = v.getClass();
        return c.isHidden() || c.isSynthetic() || v instanceof Codec || v instanceof com.mojang.serialization.MapCodec
            || v instanceof net.minecraft.network.codec.StreamCodec || v instanceof org.slf4j.Logger || v instanceof Thread
            || v instanceof java.util.concurrent.Executor || v instanceof ThreadLocal || v instanceof net.minecraft.util.RandomSource
            || c.getName().startsWith("java.util.concurrent.atomic.");
    }

    static boolean isOpaque(JsonElement e) {
        return e == null || (e.isJsonPrimitive() && e.getAsJsonPrimitive().isString() && e.getAsString().startsWith("?"));
    }

    private static JsonElement unknown(Object v) {
        return new JsonPrimitive("?" + (v.getClass().isHidden() ? "lambda" : simple(v.getClass())));
    }

    private static JsonElement num(double d) {
        return Double.isFinite(d) ? new JsonPrimitive(d) : new JsonPrimitive(String.valueOf(d));
    }

    static JsonArray box(AABB a) {
        JsonArray out = new JsonArray();
        for (double d : new double[] {a.minX, a.minY, a.minZ, a.maxX, a.maxY, a.maxZ}) out.add(d);
        return out;
    }

    static JsonArray ints(int[] v) {
        JsonArray out = new JsonArray();
        for (int x : v) out.add(x);
        return out;
    }

    private static JsonElement enc(Object v, int depth) {
        if (v == null) return JsonNull.INSTANCE;
        if (++nodes > MAX_NODES) {
            if (nodes == MAX_NODES + 1)
                problem("an object graph (at a " + simple(v.getClass()) + ") was cut off: make that type opaque or give it a short form");
            return new JsonPrimitive("?truncated");
        }
        if (opaque(v)) return null;
        if (v instanceof Boolean b) return new JsonPrimitive(b);
        if (v instanceof Float f) return Float.isFinite(f) ? new JsonPrimitive(f) : new JsonPrimitive(String.valueOf(f));
        if (v instanceof Double d) return num(d);
        if (v instanceof Number n) return new JsonPrimitive(n);
        if (v instanceof CharSequence || v instanceof Character) return new JsonPrimitive(v.toString());
        if (v instanceof Enum<?> e) return new JsonPrimitive(e.name());
        if (v instanceof Class<?> c) return new JsonPrimitive(c.getName());
        if (v instanceof Identifier || v instanceof UUID) return new JsonPrimitive(v.toString());
        if (v instanceof ResourceKey<?> k) return new JsonPrimitive(k.identifier().toString());
        if (v instanceof TagKey<?> t) return new JsonPrimitive("#" + t.location());
        if (v instanceof Holder<?> h) {
            Optional<? extends ResourceKey<?>> key = h.unwrapKey();
            if (key.isPresent()) return new JsonPrimitive(key.get().identifier().toString());
            return h.isBound() ? enc(h.value(), depth) : new JsonPrimitive("?unbound");
        }
        if (v instanceof HolderSet<?> set) {
            Either<? extends TagKey<?>, ? extends List<? extends Holder<?>>> u = set.unwrap();
            return u.left().isPresent() ? enc(u.left().get(), depth) : list(u.right().get(), depth, false);
        }
        String entry = ENTRY_NAMES.get(v);
        if (entry != null) return new JsonPrimitive(entry);
        if (v instanceof Registry<?> r) return new JsonPrimitive("registry " + r.key().identifier());
        if (v instanceof IdMap<?>) return null;  // Block.BLOCK_STATE_REGISTRY: an index, not data
        if (v instanceof BlockState s) return tagged("state", Block.getId(s));
        if (v instanceof FluidState s) return tagged("fluid_state", Fluid.FLUID_STATE_REGISTRY.getId(s));
        if (v instanceof Optional<?> o) return o.isPresent() ? enc(o.get(), depth) : JsonNull.INSTANCE;
        if (v instanceof OptionalInt o) return o.isPresent() ? new JsonPrimitive(o.getAsInt()) : JsonNull.INSTANCE;
        if (v instanceof OptionalLong o) return o.isPresent() ? new JsonPrimitive(o.getAsLong()) : JsonNull.INSTANCE;
        if (v instanceof OptionalDouble o) return o.isPresent() ? num(o.getAsDouble()) : JsonNull.INSTANCE;
        if (v instanceof Either<?, ?> e) return enc(e.left().isPresent() ? e.left().get() : e.right().get(), depth);
        if (v instanceof Pair<?, ?> p) return pair(p.getFirst(), p.getSecond(), depth);
        if (v instanceof Map.Entry<?, ?> e) return pair(e.getKey(), e.getValue(), depth);
        if (v instanceof Vec3 p) return doubles(p.x, p.y, p.z);
        if (v instanceof Vec3i p) return ints(new int[] {p.getX(), p.getY(), p.getZ()});
        if (v instanceof AABB a) return box(a);
        if (v instanceof VoxelShape s) {
            JsonArray out = new JsonArray();
            for (AABB a : s.toAabbs()) out.add(box(a));
            return out;
        }
        if (v instanceof Property<?> p) {
            JsonObject o = new JsonObject();
            o.addProperty("property", p.getName());
            o.add("values", propertyValues(p));
            return o;
        }
        if (v instanceof FeatureFlagSet f) {
            List<String> names = new ArrayList<>();
            for (Identifier id : FeatureFlags.REGISTRY.toNames(f)) names.add(id.toString());
            Collections.sort(names);
            JsonArray out = new JsonArray();
            for (String n : names) out.add(n);
            return out;
        }
        if (v instanceof ItemStack s && s.isEmpty()) return JsonNull.INSTANCE;  // its codec refuses an empty stack
        if (v instanceof Component c) return viaCodec(ComponentSerialization.CODEC, c, v);
        if (v instanceof Map<?, ?> m) return map(m, depth);
        if (v instanceof Collection<?> c) return list(c, depth, !(c instanceof List || c instanceof SortedSet || c instanceof EnumSet));
        if (v.getClass().isArray()) {
            JsonArray out = new JsonArray();
            int n = Array.getLength(v);
            if (n > 65536) return new JsonPrimitive("?array[" + n + "]");
            for (int i = 0; i < n; ++i) out.add(orUnknown(enc(Array.get(v, i), depth + 1), Array.get(v, i)));
            return out;
        }
        Class<?> c = v.getClass();
        if (!vanilla(c) && !c.getName().startsWith("com.mojang.")) return unknown(v);
        Optional<Codec<Object>> codec = CODECS.computeIfAbsent(c, Dump::findCodec);
        JsonElement named = null;
        if (codec.isPresent()) {
            JsonElement e = viaCodec(codec.get(), v, null);
            // A codec that writes only a name (BlockSetType "oak", TreeGrower "oak") refers to a constant
            // kept in code: what the constant holds is the data, so its fields are preferred below.
            if (e != null && !(e.isJsonPrimitive() && e.getAsJsonPrimitive().isString())) return e;
            named = e;
        }
        if (depth >= MAX_DEPTH || active.containsKey(v)) return named != null ? named : unknown(v);
        active.put(v, true);
        JsonObject out = new JsonObject();
        fieldsInto(out, v, depth, null);
        active.remove(v);
        if (out.size() == 0 && named != null) return named;
        // A vanilla object with no data of its own (a marker, a strategy) is still worth its name.
        if (out.size() == 0 || !(v instanceof Record)) {
            JsonObject withClass = new JsonObject();
            withClass.addProperty("class", simple(c));
            for (Map.Entry<String, JsonElement> e : out.entrySet()) withClass.add(e.getKey(), e.getValue());
            return withClass;
        }
        return out;
    }

    private static void fieldsInto(JsonObject out, Object v, int depth, Class<?> base) {
        List<Class<?>> classes = new ArrayList<>();
        for (Class<?> k = v.getClass(); k != null && k != base; k = k.getSuperclass()) {
            if (!vanilla(k) && !k.getName().startsWith("com.mojang.")) break;
            classes.add(0, k);
        }
        for (Class<?> k : classes)
            for (Field f : k.getDeclaredFields()) {
                if (Modifier.isStatic(f.getModifiers()) || f.isSynthetic() || SKIP_FIELDS.contains(f.getName())) continue;
                Object value;
                try {
                    f.setAccessible(true);
                    value = f.get(v);
                } catch (ReflectiveOperationException | RuntimeException e) {
                    continue;
                }
                if (value == null) continue;
                JsonElement e;
                try {
                    e = enc(value, depth + 1);
                } catch (RuntimeException | LinkageError x) {
                    problem("cannot encode " + k.getName() + "." + f.getName() + ": " + x);
                    continue;
                }
                if (isOpaque(e) || e.isJsonNull()) continue;
                out.add(out.has(f.getName()) ? simple(k) + "." + f.getName() : f.getName(), e);
            }
    }

    private static <T extends Comparable<T>> JsonArray propertyValues(Property<T> p) {
        JsonArray out = new JsonArray();
        for (T value : p.getPossibleValues()) out.add(p.getName(value));
        return out;
    }

    private static JsonElement orUnknown(JsonElement e, Object v) {
        return e == null ? unknown(v) : e;
    }

    private static JsonObject tagged(String key, int id) {
        JsonObject o = new JsonObject();
        o.addProperty(key, id);
        return o;
    }

    private static JsonArray doubles(double... v) {
        JsonArray out = new JsonArray();
        for (double d : v) out.add(num(d));
        return out;
    }

    private static JsonArray pair(Object a, Object b, int depth) {
        JsonArray out = new JsonArray();
        out.add(orUnknown(enc(a, depth + 1), a));
        out.add(orUnknown(enc(b, depth + 1), b));
        return out;
    }

    private static JsonArray list(Collection<?> c, int depth, boolean sort) {
        List<JsonElement> items = new ArrayList<>();
        for (Object o : c) items.add(orUnknown(enc(o, depth + 1), o));
        if (sort) items.sort(Dump::order);  // HashSet / Set.of order changes between runs
        JsonArray out = new JsonArray();
        for (JsonElement e : items) out.add(e);
        return out;
    }

    private static JsonElement map(Map<?, ?> m, int depth) {
        // Only a map that orders itself is in a reproducible order: an insertion-ordered one may have been
        // filled from a Map.of() or a HashMap, whose order changes between runs (Inventory.EQUIPMENT_SLOT_MAPPING).
        boolean ordered = m instanceof SortedMap || m instanceof EnumMap;
        List<JsonElement[]> rows = new ArrayList<>();
        boolean stringKeys = true;
        for (Map.Entry<?, ?> e : m.entrySet()) {
            JsonElement k = orUnknown(enc(e.getKey(), depth + 1), e.getKey());
            JsonElement val = orUnknown(enc(e.getValue(), depth + 1), e.getValue());
            stringKeys &= k.isJsonPrimitive();
            rows.add(new JsonElement[] {k, val});
        }
        if (!ordered) rows.sort((a, b) -> order(a[0], b[0]));
        if (stringKeys) {
            JsonObject out = new JsonObject();
            for (JsonElement[] r : rows) {
                if (out.has(r[0].getAsString())) return rowsOf(rows);  // two keys with one name: keep both
                out.add(r[0].getAsString(), r[1]);
            }
            return out;
        }
        return rowsOf(rows);
    }

    /** Numbers by value, everything else by its JSON text. */
    private static int order(JsonElement a, JsonElement b) {
        if (a.isJsonPrimitive() && b.isJsonPrimitive() && a.getAsJsonPrimitive().isNumber() && b.getAsJsonPrimitive().isNumber())
            return Double.compare(a.getAsDouble(), b.getAsDouble());
        return a.toString().compareTo(b.toString());
    }

    private static JsonArray rowsOf(List<JsonElement[]> rows) {
        JsonArray out = new JsonArray();
        for (JsonElement[] r : rows) {
            JsonArray p = new JsonArray();
            p.add(r[0]);
            p.add(r[1]);
            out.add(p);
        }
        return out;
    }

    @SuppressWarnings("unchecked")
    private static Optional<Codec<Object>> findCodec(Class<?> c) {
        // CODEC on the class or on something it extends: a dispatch codec of the base type takes the subtype.
        List<Class<?>> where = new ArrayList<>();
        for (Class<?> k = c; k != null && vanilla(k); k = k.getSuperclass()) {
            where.add(k);
            where.addAll(Arrays.asList(k.getInterfaces()));
        }
        for (Class<?> k : where)
            for (String name : new String[] {"CODEC", "DIRECT_CODEC"})
                try {
                    Field f = k.getDeclaredField(name);
                    if (!Modifier.isStatic(f.getModifiers()) || !Codec.class.isAssignableFrom(f.getType())) continue;
                    f.setAccessible(true);
                    Object codec = f.get(null);
                    if (codec != null) return Optional.of((Codec<Object>) codec);
                } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
                    // no codec here
                }
        return Optional.empty();
    }

    /** Encode through a vanilla codec; null when it is not a codec for this value after all. */
    @SuppressWarnings("unchecked")
    private static <T> JsonElement viaCodec(Codec<T> codec, Object v, Object fallbackName) {
        try {
            DataResult<JsonElement> r = codec.encodeStart(ops, (T) v);
            Optional<JsonElement> ok = r.result();
            if (ok.isPresent()) return ok.get();
        } catch (RuntimeException | LinkageError e) {
            // fall through: CODEC was for another type, or needs context that is not there
        }
        return fallbackName == null ? null : unknown(fallbackName);
    }

    // ------------------------------------------------------------------------------------ shapes

    /** Canonical ids for shapes, shared by every shape column; id 0 is the empty shape. */
    static final class ShapeTable {
        private final Map<String, Integer> ids = new LinkedHashMap<>();
        final List<List<AABB>> boxes = new ArrayList<>();

        ShapeTable() {
            ids.put("", 0);
            boxes.add(List.of());
        }

        int id(VoxelShape s) {
            // optimize() merges boxes, so shapes built differently but geometrically equal share an id.
            return s.isEmpty() ? 0 : id(s.optimize().toAabbs());
        }

        int id(List<AABB> raw) {
            if (raw.isEmpty()) return 0;
            List<AABB> b = new ArrayList<>(raw);
            b.sort((x, y) -> Arrays.compare(new double[] {x.minY, x.minZ, x.minX, x.maxY, x.maxZ, x.maxX},
                new double[] {y.minY, y.minZ, y.minX, y.maxY, y.maxZ, y.maxX}));
            StringBuilder key = new StringBuilder();
            for (AABB a : b) key.append(a.minX).append(',').append(a.minY).append(',').append(a.minZ).append(',')
                .append(a.maxX).append(',').append(a.maxY).append(',').append(a.maxZ).append(';');
            Integer have = ids.get(key.toString());
            if (have != null) return have;
            ids.put(key.toString(), boxes.size());
            boxes.add(b);
            return boxes.size() - 1;
        }

        JsonArray json() {
            JsonArray out = new JsonArray();
            for (List<AABB> shape : boxes) {
                JsonArray s = new JsonArray();
                for (AABB a : shape) s.add(box(a));
                out.add(s);
            }
            return out;
        }
    }

    // ------------------------------------------------------------------------------------ output

    /**
     * One file per section. Top-level members go one per line, and so do the elements of top-level
     * arrays and the members of top-level objects: `grep '"minecraft:oak_stairs"' blocks.json` is one
     * whole record, and a diff between two versions is readable.
     */
    static void write(Path file, JsonObject root) throws IOException {
        try (Writer w = Files.newBufferedWriter(file, StandardCharsets.UTF_8)) {
            w.write("{\n");
            int i = 0;
            for (Map.Entry<String, JsonElement> e : root.entrySet()) {
                w.write(new JsonPrimitive(e.getKey()) + ": ");
                JsonElement v = e.getValue();
                if (v.isJsonArray() && v.getAsJsonArray().size() > 0 && !v.getAsJsonArray().get(0).isJsonPrimitive()) {
                    w.write("[\n");
                    int j = 0, n = v.getAsJsonArray().size();
                    for (JsonElement item : v.getAsJsonArray()) w.write(" " + item + (++j < n ? ",\n" : "\n"));
                    w.write("]");
                } else if (v.isJsonObject() && v.getAsJsonObject().size() > 0) {
                    w.write("{\n");
                    int j = 0, n = v.getAsJsonObject().size();
                    for (Map.Entry<String, JsonElement> m : v.getAsJsonObject().entrySet())
                        w.write(" " + new JsonPrimitive(m.getKey()) + ": " + m.getValue() + (++j < n ? ",\n" : "\n"));
                    w.write("}");
                } else {
                    w.write(v.toString());
                }
                w.write(++i < root.size() ? ",\n" : "\n");
            }
            w.write("}\n");
        }
    }
}
