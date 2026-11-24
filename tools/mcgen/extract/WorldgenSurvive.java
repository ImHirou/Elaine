// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen (survive): what BlockState.canSurvive answers for the blocks world generation
// places - asked of the jar, state by state.
//
// A feature that puts a plant somewhere asks the plant whether it can stay (SimpleBlockFeature, the
// predicate would_survive), and the answer is the block's own code: forty classes of it. Most of
// them ask one thing only - what one block is, the one below for most. For those nothing is ported: the method is
// run here against every block state there is, and what comes of it is a set of states
// (survive.json), which src/worldgen looks a state up in. The rest - a cactus looks at four sides,
// a mushroom at the light - are named with their class, and are code there
// (src/world/block_rules.hpp), written from the class.
//
// How a state is found to be of the first kind: canSurvive is run in worlds of blocks drawn at
// random, with a level that writes down which blocks it was asked about. Only one, ever, and no
// other question: then that block is all it can depend on - and while every state is tried in
// its place, the level goes on watching that nothing else is asked.
//
//   {"version": ..., "sets": ["<hex: a bit a state, state 0 the lowest bit of the first byte>", ...],
//    "states": {"<state id>": {"kind": "always" | "never" | "below" | "code", "set": n, "class": "CactusBlock",
//                              "at": [dx, dy, dz], "asks": [[dx, dy, dz], ...]}}}
//   kind "one": it stays if the block at `at` is in set `set`.

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.HashMap;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.TreeSet;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.LevelReader;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;

final class WorldgenSurvive {
    private WorldgenSurvive() {}

    static final BlockPos AT = new BlockPos(0, 64, 0);

    /** A world of a few blocks around AT - anything else is `rest` - that writes down what it is asked. */
    static final class Probe implements InvocationHandler {
        final Map<BlockPos, BlockState> blocks = new HashMap<>();
        BlockState rest = Blocks.AIR.defaultBlockState();
        final TreeSet<BlockPos> asked = new TreeSet<>();
        boolean other;  // a question that is not about a block
        Integer light;  // what getRawBrightness answers; none: that is such a question
        final LevelReader level = (LevelReader) java.lang.reflect.Proxy.newProxyInstance(
            WorldgenSurvive.class.getClassLoader(), new Class<?>[] {LevelReader.class}, this);

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            String name = method.getName();
            if ((name.equals("getBlockState") || name.equals("getFluidState")) && args.length == 1 && args[0] instanceof BlockPos pos) {
                asked.add(pos.immutable());
                BlockState state = blocks.getOrDefault(pos, rest);
                return name.equals("getBlockState") ? state : state.getFluidState();
            }
            if (name.equals("getRawBrightness") && light != null) return light;
            // What an interface says in terms of its other methods comes back here.
            if (method.isDefault()) return InvocationHandler.invokeDefault(proxy, method, args);
            if (method.getDeclaringClass() == Object.class) return method.invoke(this, args);
            // The height of a world is no question about what is in it: a world as tall as the overworld.
            if (name.equals("getMinY")) return -64;
            if (name.equals("getHeight") && (args == null || args.length == 0)) return 384;
            // What a block's shape asks of the world around it - a shulker box its block entity - is answered as
            // for a block that stands alone: the tables of shapes are made that way (mcgen.py gamedata).
            if (method.getDeclaringClass().isInstance(EmptyBlockGetter.INSTANCE)) return method.invoke(EmptyBlockGetter.INSTANCE, args);
            other = true;
            throw new UnsupportedOperationException(name);
        }

        /** canSurvive, or null where the block asked what this world cannot say. */
        Boolean survives(BlockState state) {
            try {
                return state.canSurvive(level, AT);
            } catch (RuntimeException | Error e) {
                other = true;
                return null;
            }
        }
    }

    /** Every block state a file of data/minecraft/worldgen names, of features, placed features and state providers. */
    static List<Block> named() throws Exception {
        TreeMap<String, Block> found = new TreeMap<>();
        java.util.ArrayDeque<com.google.gson.JsonElement> open = new java.util.ArrayDeque<>();
        for (String kind : new String[] {"worldgen/placed_feature", "worldgen/feature", "worldgen/block_state_provider"})
            for (var file : WorldgenFeatures.resources.listResources(kind, id -> id.getPath().endsWith(".json")).entrySet())
                try (java.io.Reader reader = file.getValue().openAsReader()) {
                    open.add(com.google.gson.JsonParser.parseReader(reader));
                }
        while (!open.isEmpty()) {
            com.google.gson.JsonElement e = open.poll();
            if (e.isJsonArray()) e.getAsJsonArray().forEach(open::add);
            if (e.isJsonObject()) e.getAsJsonObject().asMap().values().forEach(open::add);
            if (!e.isJsonPrimitive() || !e.getAsJsonPrimitive().isString()) continue;
            if (e.getAsString().startsWith("#")) {
                // A tag of blocks - what a provider draws one of (RandomBlockProvider): every block of it.
                Identifier tag = Identifier.tryParse(e.getAsString().substring(1));
                if (tag != null)
                    BuiltInRegistries.BLOCK.get(net.minecraft.tags.TagKey.create(net.minecraft.core.registries.Registries.BLOCK, tag)).ifPresent(
                        named -> named.forEach(holder -> found.put(holder.getRegisteredName(), holder.value())));
                continue;
            }
            Identifier id = Identifier.tryParse(e.getAsString());
            if (id != null && BuiltInRegistries.BLOCK.containsKey(id)) found.put(id.toString(), BuiltInRegistries.BLOCK.getValue(id));
        }
        // ...and what features place by name in their code, where a block asks whether it can stay.
        for (Block block : new Block[] {Blocks.BAMBOO, Blocks.BAMBOO_SAPLING, Blocks.VINE, Blocks.SNOW, Blocks.KELP, Blocks.KELP_PLANT, Blocks.SEAGRASS,
                                        Blocks.TALL_SEAGRASS, Blocks.SEA_PICKLE, Blocks.COCOA, Blocks.BROWN_MUSHROOM, Blocks.RED_MUSHROOM})
            found.put(BuiltInRegistries.BLOCK.getKey(block).toString(), block);
        return new ArrayList<>(found.values());
    }

    static JsonObject survive(RegistryAccess registries) throws Exception {
        int states = Block.BLOCK_STATE_REGISTRY.size();
        java.util.Random random = new java.util.Random(20261124L);
        // Blocks to build worlds of: any state, but half the time one of a few that a rule is likely to tell apart.
        BlockState[] telling = {Blocks.AIR.defaultBlockState(), Blocks.WATER.defaultBlockState(), Blocks.STONE.defaultBlockState(),
                                Blocks.GRASS_BLOCK.defaultBlockState(), Blocks.SAND.defaultBlockState(), Blocks.DIRT.defaultBlockState(),
                                Blocks.LAVA.defaultBlockState(), Blocks.FARMLAND.defaultBlockState(), Blocks.ICE.defaultBlockState()};
        java.util.function.Supplier<BlockState> any = () ->
            random.nextBoolean() ? telling[random.nextInt(telling.length)] : Block.stateById(random.nextInt(states));
        List<String> sets = new ArrayList<>();
        Map<String, Integer> setIndex = new HashMap<>();
        JsonObject out = new JsonObject();
        int always = 0, below = 0, code = 0;
        for (Block block : named()) {
            // (A set is worked out for one state of a block and tried on the others: few blocks stay by what state they are in.)
            Map<String, byte[]> made = new LinkedHashMap<>();
            for (BlockState state : block.getStateDefinition().getPossibleStates()) {
                Probe probe = new Probe();
                boolean yes = false, no = false;
                for (int world = 0; world < 48 && !probe.other; ++world) {
                    probe.blocks.clear();
                    probe.rest = world < 9 ? telling[world] : any.get();
                    if (world >= 9)
                        for (int dx = -1; dx <= 1; ++dx)
                            for (int dy = -2; dy <= 2; ++dy)
                                for (int dz = -1; dz <= 1; ++dz) probe.blocks.put(AT.offset(dx, dy, dz), any.get());
                    Boolean answer = probe.survives(state);
                    if (answer == null) break;
                    yes |= answer;
                    no |= !answer;
                }
                JsonObject of = new JsonObject();
                JsonArray asks = new JsonArray();
                for (BlockPos pos : probe.asked) {
                    JsonArray offset = new JsonArray();
                    offset.add(pos.getX() - AT.getX());
                    offset.add(pos.getY() - AT.getY());
                    offset.add(pos.getZ() - AT.getZ());
                    asks.add(offset);
                }
                if (!probe.other && probe.asked.isEmpty() && !(yes && no)) {
                    of.addProperty("kind", yes ? "always" : "never");
                    ++always;
                } else if (!probe.other && probe.asked.size() == 1) {
                    // One block is all it asks about - the one below, mostly: every state there is, in that place.
                    BlockPos where = probe.asked.first();
                    byte[] bits = null;
                    for (Map.Entry<String, byte[]> known : made.entrySet()) {
                        if (!known.getKey().startsWith(where.toShortString() + ";")) continue;
                        byte[] candidate = known.getValue();
                        boolean same = true;
                        Probe check = new Probe();
                        for (int trial = 0; trial < 3000 && same; ++trial) {
                            int id = trial < 2000 ? random.nextInt(states) : firstSet(candidate, random.nextInt(states), states);
                            check.blocks.put(where, Block.stateById(id));
                            same = Boolean.valueOf((candidate[id >> 3] >> (id & 7) & 1) != 0).equals(check.survives(state));
                        }
                        if (same) {
                            bits = candidate;
                            break;
                        }
                    }
                    if (bits == null) {
                        bits = new byte[(states + 7) / 8];
                        Probe all = new Probe();
                        for (int id = 0; id < states; ++id) {
                            all.blocks.put(where, Block.stateById(id));
                            Boolean answer = all.survives(state);
                            if (answer == null || all.asked.size() != 1 || !all.asked.first().equals(where)) {
                                bits = null;
                                break;
                            }
                            if (answer) bits[id >> 3] |= (byte) (1 << (id & 7));
                        }
                        if (bits != null) made.put(where.toShortString() + ";" + Block.getId(state), bits);
                    }
                    if (bits != null) {
                        String hex = java.util.HexFormat.of().formatHex(bits);
                        Integer index = setIndex.get(hex);
                        if (index == null) {
                            setIndex.put(hex, index = sets.size());
                            sets.add(hex);
                        }
                        of.addProperty("kind", "one");
                        of.addProperty("set", index);
                        of.add("at", asks.get(0));
                        ++below;
                    }
                }
                if (!of.has("kind")) {
                    of.addProperty("kind", "code");
                    of.add("asks", asks);
                    ++code;
                }
                of.addProperty("class", block.getClass().getSimpleName());
                out.add(Integer.toString(Block.getId(state)), of);
            }
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        JsonArray setsOut = new JsonArray();
        sets.forEach(setsOut::add);
        root.add("sets", setsOut);
        root.add("states", out);
        System.err.println("worldgen: survive: " + out.size() + " states of the blocks worldgen names: " + always + " stay or go whatever is around, " + below
            + " by one block (" + sets.size() + " sets), " + code + " by more than that");
        return root;
    }

    /** A sequence of numbers that is the same here and in tests/unit/feature_test.cpp (SplitMix64). */
    static final class Mix {
        long x;

        Mix(long seed) {
            x = seed;
        }

        long next() {
            x += 0x9E3779B97F4A7C15L;
            long z = x;
            z = (z ^ (z >>> 30)) * 0xBF58476D1CE4E5B9L;
            z = (z ^ (z >>> 27)) * 0x94D049BB133111EBL;
            return z ^ (z >>> 31);
        }
    }

    /**
     * For the golden script: the rules src/worldgen has of canSurvive (survive_rules.txt, which `mcgen.py generate`
     * writes: a state and its rule a line), each held to the method itself in worlds drawn at random - the blocks
     * within one of the place and two above and below it, any block state or one of a few that a rule is likely to
     * tell apart, and light or none. Of each rule two states; of each state so many worlds, and the answers a bit each.
     */
    static void golden(WorldgenGolden.Out o) throws Exception {
        int states = Block.BLOCK_STATE_REGISTRY.size();
        BlockState[] telling = {Blocks.AIR.defaultBlockState(), Blocks.WATER.defaultBlockState(), Blocks.STONE.defaultBlockState(),
                                Blocks.GRASS_BLOCK.defaultBlockState(), Blocks.SAND.defaultBlockState(), Blocks.DIRT.defaultBlockState(),
                                Blocks.LAVA.defaultBlockState(), Blocks.MYCELIUM.defaultBlockState(), Blocks.CLAY.defaultBlockState(),
                                Blocks.CACTUS.defaultBlockState(), Blocks.SUGAR_CANE.defaultBlockState(), Blocks.BIG_DRIPLEAF_STEM.defaultBlockState(),
                                Blocks.BIG_DRIPLEAF.defaultBlockState(), Blocks.MOSS_BLOCK.defaultBlockState(), Blocks.CAVE_AIR.defaultBlockState(),
                                Blocks.ICE.defaultBlockState()};
        o.i32(telling.length);
        for (BlockState state : telling) o.i32(Block.getId(state));
        java.nio.file.Path list = WorldgenRef.out.resolve("survive_rules.txt");
        if (!java.nio.file.Files.exists(list)) {
            WorldgenRef.problem("no " + list + ": `mcgen.py generate` writes which rule each state stays by");
            return;
        }
        Map<Integer, List<Integer>> byRule = new TreeMap<>();
        for (String line : java.nio.file.Files.readAllLines(list)) {
            String[] part = line.trim().split(" ");
            List<Integer> of = byRule.computeIfAbsent(Integer.parseInt(part[1]), unused -> new ArrayList<>());
            if (of.size() < 2) of.add(Integer.parseInt(part[0]));
        }
        int entries = 0;
        for (List<Integer> of : byRule.values()) entries += of.size();
        o.i32(entries);
        final int trials = 400;
        for (Map.Entry<Integer, List<Integer>> rule : byRule.entrySet())
            for (int id : rule.getValue()) {
                BlockState state = Block.stateById(id);
                o.i32(id);
                o.i32(rule.getKey());
                o.i32(trials);
                Probe probe = new Probe();
                byte[] answers = new byte[(trials + 7) / 8];
                for (int trial = 0; trial < trials; ++trial) {
                    Mix mix = new Mix((long) id * 1000003L + trial);
                    java.util.function.Supplier<BlockState> pick = () -> {
                        long r = mix.next();
                        return (r & 1) != 0 ? telling[(int) ((r >>> 1) % telling.length)] : Block.stateById((int) ((r >>> 1) % states));
                    };
                    probe.light = (mix.next() & 1) != 0 ? 15 : 0;
                    probe.rest = pick.get();
                    probe.blocks.clear();
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dy = -2; dy <= 2; ++dy)
                            for (int dz = -1; dz <= 1; ++dz) probe.blocks.put(AT.offset(dx, dy, dz), pick.get());
                    Boolean answer = probe.survives(state);
                    if (answer == null) {
                        WorldgenRef.problem(state + " asks what a world of blocks and light cannot say");
                        return;
                    }
                    if (answer) answers[trial >> 3] |= (byte) (1 << (trial & 7));
                }
                for (byte b : answers) o.u8(b & 0xFF);
            }
        System.err.println("worldgen: golden features: " + entries + " block states asked whether they can stay, " + trials + " worlds each");
    }

    /** A state that is in the set, at or after `from` (or any, where the set is empty). */
    static int firstSet(byte[] bits, int from, int states) {
        for (int i = 0; i < states; ++i) {
            int id = (from + i) % states;
            if ((bits[id >> 3] >> (id & 7) & 1) != 0) return id;
        }
        return from;
    }
}
