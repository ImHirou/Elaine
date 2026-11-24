// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen worldgen (features): vanilla's own `features` status, run without a server.
//
// A chunk is decorated by ChunkGenerator.applyBiomeDecoration over a WorldGenRegion - and a region
// wants a ServerLevel, which wants a server. What stands here instead is the least that lets the
// *real* WorldGenRegion work: a level, its chunk source and a server made without their
// constructors (nothing a constructor would have set up is asked for), answering the few questions
// a region and the features have with what a world of this seed would say. Every method of the
// region is vanilla's own: what it refuses, what it marks and which heightmaps a write moves is
// not written down here a second time.
//
//   census   what vanilla's features do that a generator needs a rule for: reads beyond the nine
//            chunks a decoration may write to, writes that are refused, questions about light
//   replay   chunks decorated in an order that is given - the order a vanilla server took, by its
//            flight recording - and what each chunk is afterwards, to be held against what that
//            server saved (tools/vanilla/feature_probe.py)

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.lang.reflect.Field;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.EnumSet;
import java.util.HashMap;
import java.util.List;
import java.util.Map;
import java.util.TreeMap;
import java.util.concurrent.CompletableFuture;
import java.util.concurrent.ConcurrentHashMap;
import java.util.function.Supplier;
import net.minecraft.SharedConstants;
import net.minecraft.SystemReport;
import net.minecraft.commands.Commands;
import net.minecraft.core.BlockPos;
import net.minecraft.core.RegistryAccess;
import net.minecraft.core.registries.Registries;
import net.minecraft.resources.FileToIdConverter;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.MinecraftServer;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.level.GenerationChunkHolder;
import net.minecraft.server.level.ServerChunkCache;
import net.minecraft.server.level.ServerLevel;
import net.minecraft.server.level.WorldGenRegion;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.packs.resources.CloseableResourceManager;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import net.minecraft.server.permissions.PermissionSet;
import net.minecraft.server.players.NameAndId;
import net.minecraft.util.StaticCache2D;
import net.minecraft.util.Util;
import net.minecraft.util.datafix.DataFixers;
import net.minecraft.util.debugchart.SampleLogger;
import net.minecraft.world.flag.FeatureFlagSet;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.ChunkPos;
import net.minecraft.world.level.DataPackConfig;
import net.minecraft.world.level.LightLayer;
import net.minecraft.world.level.StructureManager;
import net.minecraft.world.level.WorldDataConfiguration;
import net.minecraft.world.level.biome.BiomeManager;
import net.minecraft.world.level.biome.BiomeResolver;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.chunk.ChunkAccess;
import net.minecraft.world.level.chunk.ChunkGenerator;
import net.minecraft.world.level.chunk.LightChunk;
import net.minecraft.world.level.chunk.LightChunkGetter;
import net.minecraft.world.level.chunk.ProtoChunk;
import net.minecraft.world.level.chunk.status.ChunkPyramid;
import net.minecraft.world.level.chunk.status.ChunkStatus;
import net.minecraft.world.level.chunk.status.ChunkStep;
import net.minecraft.world.level.dimension.BuiltinDimensionTypes;
import net.minecraft.world.level.dimension.DimensionType;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.levelgen.NoiseChunk;
import net.minecraft.world.level.levelgen.RandomState;
import net.minecraft.world.level.levelgen.WorldOptions;
import net.minecraft.world.level.levelgen.blending.Blender;
import net.minecraft.world.level.levelgen.material.rule.MaterialRule;
import net.minecraft.world.level.levelgen.structure.templatesystem.StructureTemplateManager;
import net.minecraft.world.level.levelgen.structure.templatesystem.loader.ResourceManagerTemplateSource;
import net.minecraft.world.level.lighting.LayerLightEventListener;
import net.minecraft.world.level.lighting.LevelLightEngine;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.storage.LevelData;

final class WorldgenFeatures {
    private WorldgenFeatures() {}

    // ------------------------------------------------------------------------------- the game

    /** The resources the game was loaded from: structure templates are read from them when a feature asks. */
    static CloseableResourceManager resources;

    /** GameData.load(), with the resource manager left open. */
    static GameData.Env load() throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        PackRepository packs = ServerPacksSource.createVanillaTrustedRepository();
        packs.reload();
        FeatureFlagSet features = FeatureFlags.DEFAULT_FLAGS;
        WorldDataConfiguration data = new WorldDataConfiguration(new DataPackConfig(List.of("vanilla"), List.of()), features);
        WorldLoader.InitConfig init = new WorldLoader.InitConfig(new WorldLoader.PackConfig(packs, data, false, true),
            Commands.CommandSelection.DEDICATED, LevelBasedPermissionSet.OWNER);
        GameData.Loaded loaded = Util.blockUntilDone(executor -> WorldLoader.load(init,
            context -> new WorldLoader.DataLoadOutput<>(Boolean.TRUE, context.datapackDimensions()),
            (resourceManager, serverResources, layers, cookie) -> {
                resources = resourceManager;
                return new GameData.Loaded(serverResources, layers);
            }, Util.backgroundExecutor(), executor)).get();
        return new GameData.Env(loaded.layers().compositeAccess(), loaded.resources(), features);
    }

    // ------------------------------------------------------------------------------- stand-ins

    private static final sun.misc.Unsafe UNSAFE;

    static {
        try {
            Field f = sun.misc.Unsafe.class.getDeclaredField("theUnsafe");
            f.setAccessible(true);
            UNSAFE = (sun.misc.Unsafe) f.get(null);
        } catch (ReflectiveOperationException e) {
            throw new ExceptionInInitializerError(e);
        }
    }

    /** An object of a class whose constructor is not run: every field of vanilla's is null or zero. */
    @SuppressWarnings("unchecked")
    static <T> T blank(Class<T> k) throws Exception {
        return (T) UNSAFE.allocateInstance(k);
    }

    static void set(Object o, String name, Object value) throws Exception {
        Field f = null;
        for (Class<?> k = o.getClass(); f == null && k != null; k = k.getSuperclass())
            try {
                f = k.getDeclaredField(name);
            } catch (NoSuchFieldException e) {
                // (a superclass has it)
            }
        if (f == null) throw new NoSuchFieldException(o.getClass().getName() + "." + name);
        f.setAccessible(true);
        f.set(o, value);
    }

    /** The server a feature reaches for its structure templates. */
    static final class FakeServer extends MinecraftServer {
        StructureTemplateManager templates;
        RegistryAccess.Frozen access;

        FakeServer() {
            super(null, null, null, null, null, null, null, null, null, false, null);
        }

        @Override
        public StructureTemplateManager getStructureTemplateManager() {
            return templates;
        }

        @Override
        public RegistryAccess.Frozen registryAccess() {
            return access;
        }

        @Override
        protected boolean initServer() {
            return false;
        }

        @Override
        public LevelBasedPermissionSet operatorUserPermissions() {
            return LevelBasedPermissionSet.OWNER;
        }

        @Override
        public PermissionSet getFunctionCompilationPermissions() {
            return LevelBasedPermissionSet.OWNER;
        }

        @Override
        public boolean shouldRconBroadcast() {
            return false;
        }

        @Override
        protected SampleLogger getTickTimeLogger() {
            return null;
        }

        @Override
        public boolean isTickTimeLoggingEnabled() {
            return false;
        }

        @Override
        public SystemReport fillServerSystemReport(SystemReport report) {
            return report;
        }

        @Override
        public boolean isDedicatedServer() {
            return true;
        }

        @Override
        public int getRateLimitPacketsPerSecond() {
            return 0;
        }

        @Override
        public int getCommandSpamThresholdSeconds() {
            return 0;
        }

        @Override
        public int getChatSpamThresholdSeconds() {
            return 0;
        }

        @Override
        public boolean useNativeTransport() {
            return false;
        }

        @Override
        public boolean isPublished() {
            return false;
        }

        @Override
        public boolean shouldInformAdmins() {
            return false;
        }

        @Override
        public boolean isSingleplayerOwner(NameAndId who) {
            return false;
        }

        @Override
        public int getMaxPlayers() {
            return 0;
        }
    }

    static final class FakeChunkCache extends ServerChunkCache {
        RandomState state;
        ChunkGenerator generator;

        FakeChunkCache() {
            super(null, null, null, null, null, null, 0, 0, false, null);
        }

        @Override
        public RandomState randomState() {
            return state;
        }

        @Override
        public ChunkGenerator getGenerator() {
            return generator;
        }
    }

    /** What a WorldGenRegion and the features ask a level, answered for a world of one seed. */
    static final class FakeLevel extends ServerLevel {
        long seed;
        LevelData data;
        FakeChunkCache source;
        DimensionType type;
        RegistryAccess access;
        FeatureFlagSet flags;
        FakeServer server;
        LevelLightEngine light;
        int seaLevel;

        FakeLevel() {
            super(null, null, null, null, null, null, false, 0L, null, false);
        }

        @Override
        public long getSeed() {
            return seed;
        }

        @Override
        public LevelData getLevelData() {
            return data;
        }

        @Override
        public ServerChunkCache getChunkSource() {
            return source;
        }

        @Override
        public DimensionType dimensionType() {
            return type;
        }

        /** LevelReader.getNoiseBiome falls back on it for a chunk a region does not have - and a region throws before that. */
        @Override
        public BiomeResolver uncachedBiomeResolver() {
            return (x, y, z) -> {
                throw new IllegalStateException("a biome was asked of no chunk");
            };
        }

        @Override
        public RegistryAccess registryAccess() {
            return access;
        }

        @Override
        public FeatureFlagSet enabledFeatures() {
            return flags;
        }

        @Override
        public MinecraftServer getServer() {
            return server;
        }

        @Override
        public int getSeaLevel() {
            return seaLevel;
        }

        @Override
        public LevelLightEngine getLightEngine() {
            return light;
        }

        /** The real one hands the change to the server's thread: points of interest are not a chunk's blocks. */
        @Override
        public void updatePOIOnBlockStateChange(BlockPos pos, BlockState oldState, BlockState newState) {}
    }

    /** What ChunkMap keeps a chunk in while it is generated; a region takes its chunks out of these. */
    static final class Slot extends GenerationChunkHolder {
        final ChunkAccess chunk;

        Slot(ChunkPos pos, ChunkAccess chunk) {
            super(pos);
            this.chunk = chunk;
        }

        @Override
        public ChunkAccess getChunkIfPresentUnchecked(ChunkStatus status) {
            return chunk;
        }

        @Override
        protected void addSaveDependency(CompletableFuture<?> sync) {}

        @Override
        public int getTicketLevel() {
            return 0;
        }

        @Override
        public int getQueueLevel() {
            return 0;
        }
    }

    // ------------------------------------------------------------------------------- census

    static final int FAR_BLOCK = 0, FAR_FLUID = 1, FAR_HEIGHT = 2, REFUSED = 3, FAR_CHUNK = 4, RAW_LIGHT = 5, LAYER_LIGHT = 6, KINDS = 7;
    static final String[] KIND = {"far_block_reads", "far_fluid_reads", "far_height_reads", "refused_writes", "far_chunk_accesses",
        "raw_brightness", "layer_light"};

    /** Per feature: how often it ran, and how often it did each of the things above - and how far out at most. */
    static final class Census {
        final Map<String, long[]> counts = new TreeMap<>();
        final Map<String, int[]> farthest = new TreeMap<>();
        final Map<String, long[]> runs = new TreeMap<>();

        void run(String feature) {
            runs.computeIfAbsent(feature, unused -> new long[1])[0]++;
        }

        void note(String feature, int kind, int distance) {
            counts.computeIfAbsent(feature, unused -> new long[KINDS])[kind]++;
            int[] far = farthest.computeIfAbsent(feature, unused -> new int[KINDS]);
            far[kind] = Math.max(far[kind], distance);
        }
    }

    /** The real region, which says what is asked of it beyond where a decoration may write. */
    static final class Region extends WorldGenRegion {
        private Stand stand;  // (null while the constructor above runs)
        int cx, cz;
        private Supplier<String> now;

        Region(Stand stand, StaticCache2D<GenerationChunkHolder> cache, ChunkAccess center) {
            super(stand.level, cache, stand.step, center);
            this.stand = stand;
            cx = center.getPos().x();
            cz = center.getPos().z();
        }

        /** "ResourceKey[minecraft:worldgen/placed_feature / minecraft:ore_dirt]" -> "ore_dirt". */
        String feature() {
            if (now == null) return "(none)";
            String s = String.valueOf(now.get());
            int at = s.lastIndexOf(" / ");
            if (at >= 0 && s.endsWith("]")) s = s.substring(at + 3, s.length() - 1);
            return s.startsWith("minecraft:") ? s.substring(10) : s;
        }

        int distance(int blockX, int blockZ) {
            return Math.max(Math.abs((blockX >> 4) - cx), Math.abs((blockZ >> 4) - cz));
        }

        private void far(int kind, int blockX, int blockZ) {
            if (stand == null) return;
            int d = distance(blockX, blockZ);
            if (d >= 2) stand.census.note(feature(), kind, d);
        }

        @Override
        public void setCurrentlyGenerating(Supplier<String> what) {
            super.setCurrentlyGenerating(what);
            now = what;
            if (what != null && stand != null) stand.census.run(feature());
        }

        @Override
        public BlockState getBlockState(BlockPos pos) {
            far(FAR_BLOCK, pos.getX(), pos.getZ());
            return super.getBlockState(pos);
        }

        @Override
        public FluidState getFluidState(BlockPos pos) {
            far(FAR_FLUID, pos.getX(), pos.getZ());
            return super.getFluidState(pos);
        }

        @Override
        public int getHeight(Heightmap.Types type, int x, int z) {
            far(FAR_HEIGHT, x, z);
            return super.getHeight(type, x, z);
        }

        @Override
        public boolean ensureCanWrite(BlockPos pos) {
            boolean can = super.ensureCanWrite(pos);
            if (!can && stand != null) stand.census.note(feature(), REFUSED, distance(pos.getX(), pos.getZ()));
            return can;
        }

        /** Every way to a chunk ends here: reads, and the marks and ticks that no write zone holds back. */
        @Override
        public ChunkAccess getChunk(int x, int z, ChunkStatus status, boolean required) {
            if (stand != null) {
                int d = Math.max(Math.abs(x - cx), Math.abs(z - cz));
                if (d >= 2) stand.census.note(feature(), FAR_CHUNK, d);
            }
            return super.getChunk(x, z, status, required);
        }
    }

    /** A light engine nothing was ever put into - what a server's is to a chunk that is not lit yet. */
    static final class Light extends LevelLightEngine {
        Stand stand;

        Light(LightChunkGetter chunks) {
            super(chunks, true, true);
        }

        @Override
        public int getRawBrightness(BlockPos pos, int darken) {
            return stand == null ? super.getRawBrightness(pos, darken) : stand.brightness(pos);
        }

        /** (Block light, asked where snow and ice would lie: none in a chunk that is not lit, whatever is beside it.) */
        @Override
        public LayerLightEventListener getLayerListener(LightLayer layer) {
            if (stand != null) stand.asked(LAYER_LIGHT);
            return super.getLayerListener(layer);
        }
    }

    // ------------------------------------------------------------------------------- the stand

    static final EnumSet<Heightmap.Types> FINAL =
        EnumSet.of(Heightmap.Types.MOTION_BLOCKING, Heightmap.Types.MOTION_BLOCKING_NO_LEAVES, Heightmap.Types.OCEAN_FLOOR, Heightmap.Types.WORLD_SURFACE);

    /** A world of proto chunks: each made as the statuses of a server make it, when it is asked for. */
    static final class Stand {
        final WorldgenRef.World world;
        final WorldgenGolden.ChunkMaker maker;
        final FakeLevel level;
        final Census census = new Census();
        final ChunkStep step = ChunkPyramid.GENERATION_PYRAMID.getStepTo(ChunkStatus.FEATURES);
        final MaterialRule rule;
        final WorldOptions options;
        final Map<Long, ProtoChunk> chunks = new HashMap<>();  // with their biomes, at least
        final Map<Long, ProtoChunk> voids = new HashMap<>();
        /**
         * What a region is given beyond the nine chunks around its centre. False: chunks with nothing in
         * them - one of the things a server's region holds there, and the one src/worldgen takes for all.
         * True: the chunks of this world as they stand, as on a server.
         */
        boolean live;
        Region region;
        int terrains, decorated, entities;

        Stand(WorldgenRef.World world) throws Exception {
            this.world = world;
            maker = new WorldgenGolden.ChunkMaker(world);
            rule = world.settings().materialRule().value();
            options = new WorldOptions(world.seed(), false, false);
            RegistryAccess registries = world.registries();

            FakeServer server = blank(FakeServer.class);
            server.access = (RegistryAccess.Frozen) registries;
            StructureTemplateManager templates = blank(StructureTemplateManager.class);
            Field lister = StructureTemplateManager.class.getDeclaredField("RESOURCE_STRUCTURE_LISTER");
            lister.setAccessible(true);
            set(templates, "structureRepository", new ConcurrentHashMap<>());
            set(templates, "sources", List.of(new ResourceManagerTemplateSource(DataFixers.getDataFixer(), registries.lookupOrThrow(Registries.BLOCK),
                resources, (FileToIdConverter) lister.get(null))));
            server.templates = templates;

            FakeChunkCache source = blank(FakeChunkCache.class);
            source.state = world.state();
            source.generator = maker.generator;

            level = blank(FakeLevel.class);
            level.seed = world.seed();
            // (a tick a feature schedules is stamped with the game time; a world that is being made has none)
            level.data = (LevelData) java.lang.reflect.Proxy.newProxyInstance(LevelData.class.getClassLoader(), new Class<?>[] {LevelData.class},
                (proxy, method, args) -> {
                    if (method.getName().equals("getGameTime")) return 0L;
                    throw new IllegalStateException("LevelData." + method.getName() + " was asked");
                });
            level.source = source;
            level.type = registries.lookupOrThrow(Registries.DIMENSION_TYPE).getOrThrow(BuiltinDimensionTypes.OVERWORLD).value();
            level.access = registries;
            level.flags = FeatureFlags.DEFAULT_FLAGS;
            level.server = server;
            level.seaLevel = maker.generator.getSeaLevel();
            Light light = new Light(new LightChunkGetter() {
                @Override
                public LightChunk getChunkForLighting(int x, int z) {
                    return null;
                }

                @Override
                public BlockGetter getLevel() {
                    return level;
                }
            });
            light.stand = this;
            level.light = light;
        }

        static long key(int x, int z) {
            return (long) x << 32 | (z & 0xFFFFFFFFL);
        }

        /** If there: every block whose light was asked for, as "centre x z, feature, block x y z" (WORLDGEN_LIGHT_LOG). */
        List<String> asked;
        /** Chunks a server had begun to light when a step ran (replay): what light is in and around them is its light thread's business. */
        final java.util.Set<Long> lighting = new java.util.HashSet<>();
        /** Per chunk, the blocks whose making asked for light where the server's answer is not known: [x, y, z]. */
        final Map<Long, List<int[]>> tainted = new HashMap<>();

        /**
         * How light it is at a block while a chunk is decorated (LevelLightEngine.getRawBrightness: only a
         * mushroom asks). A server's answer is its light thread's: 15 while no chunk around was given to
         * the light engine - and once one beside it was, 0 in a chunk that is not lit itself (the engine
         * keeps layers for the sections around a chunk it knows, empty until light is let into them:
         * LayerLightSectionStorage.updateSectionStatus, SkyLightSectionStorage.getLightValue), and later what
         * spills over from the lit chunk. The second is what a chunk finds that is made beside older ones,
         * which is every chunk a player walks towards - so that is the rule of src/worldgen: dark, and
         * what stands too bright goes when the chunk first ticks (the mushroom marks itself). A replay
         * of a server's run says 15, and marks the block where that may not be what the server said.
         */
        int brightness(BlockPos pos) {
            if (region == null) return 15;
            census.note(region.feature(), RAW_LIGHT, region.distance(pos.getX(), pos.getZ()));
            if (asked != null) asked.add(region.cx + " " + region.cz + " " + region.feature() + " " + pos.getX() + " " + pos.getY() + " " + pos.getZ());
            if (!live) return 0;
            int cx = pos.getX() >> 4, cz = pos.getZ() >> 4;
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    if (lighting.contains(key(cx + dx, cz + dz))) {
                        tainted.computeIfAbsent(key(cx, cz), unused -> new ArrayList<>()).add(new int[] {pos.getX(), pos.getY(), pos.getZ()});
                        return 15;
                    }
            return 15;
        }

        void asked(int kind) {
            if (region != null) census.note(region.feature(), kind, 0);
        }

        /** The chunk as the `biomes` status leaves it - filled on a thread that has searched for no biome before. */
        ProtoChunk withBiomes(int cx, int cz) throws Exception {
            ProtoChunk chunk = chunks.get(key(cx, cz));
            if (chunk == null) {
                ProtoChunk made = maker.chunk(cx, cz);
                WorldgenGolden.fresh(unused -> maker.doCreateBiomes.invoke(maker.generator, Blender.empty(), world.state(), made));
                made.setPersistedStatus(ChunkStatus.BIOMES);
                chunks.put(key(cx, cz), chunk = made);
            }
            return chunk;
        }

        /** ...and as `terrain` leaves it: NoiseBasedChunkGenerator.buildTerrain and what follows it there. */
        ProtoChunk terrain(int cx, int cz) throws Exception {
            ProtoChunk chunk = withBiomes(cx, cz);
            if (chunk.getPersistedStatus().isOrAfter(ChunkStatus.TERRAIN)) return chunk;
            ProtoChunk[][] around = new ProtoChunk[3][3];
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) around[dx + 1][dz + 1] = withBiomes(cx + dx, cz + dz);
            NoiseChunk noiseChunk = maker.noiseChunk(chunk);
            maker.doFill.invoke(maker.generator, noiseChunk, chunk);
            maker.buildSurface.invoke(maker.generator, chunk, noiseChunk, world.state(), maker.biomeManager(around, cx, cz), maker.possibleBiomes(around), rule);
            BiomeManager biomes = maker.biomeManager(around, cx, cz);
            WorldgenGolden.fresh(unused -> maker.generateCarvers.invoke(maker.generator, chunk, Blender.empty(), noiseChunk, world.state(), biomes, null, rule));
            noiseChunk.close();
            Heightmap.primeHeightmaps(chunk, FINAL);
            chunk.setPersistedStatus(ChunkStatus.TERRAIN);
            ++terrains;
            return chunk;
        }

        private ChunkAccess given(int cx, int cz, int x, int z) {
            if (Math.max(Math.abs(x - cx), Math.abs(z - cz)) <= 1 || live) {
                ProtoChunk chunk = chunks.get(key(x, z));
                if (chunk != null) return chunk;
            }
            return voids.computeIfAbsent(key(x, z), unused -> {
                ProtoChunk empty = maker.chunk(x, z);
                empty.setPersistedStatus(ChunkStatus.STRUCTURE_STARTS);
                return empty;
            });
        }

        /** ChunkStatusTasks.generateFeatures, and the status the step that ran it then gives the chunk. */
        void decorate(int cx, int cz) throws Exception {
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (!live) terrain(cx + dx, cz + dz);
                    ProtoChunk near = chunks.get(key(cx + dx, cz + dz));
                    if (near == null || !near.getPersistedStatus().isOrAfter(ChunkStatus.TERRAIN))
                        throw new IllegalStateException("chunk " + cx + ", " + cz + " is decorated before " + (cx + dx) + ", " + (cz + dz) + " has its terrain");
                }
            ProtoChunk center = chunks.get(key(cx, cz));
            if (center.getPersistedStatus().isOrAfter(ChunkStatus.FEATURES)) throw new IllegalStateException("chunk " + cx + ", " + cz + " is decorated twice");
            StaticCache2D<GenerationChunkHolder> cache = StaticCache2D.create(cx, cz, 8, (x, z) -> new Slot(new ChunkPos(x, z), given(cx, cz, x, z)));
            region = new Region(this, cache, center);
            try {
                maker.generator.applyBiomeDecoration(region, center, new StructureManager(region, options, null));
                Blender.generateBorderTicks(region, center);
            } finally {
                region = null;
            }
            center.setPersistedStatus(ChunkStatus.FEATURES);
            entities += center.getEntities().size();
            ++decorated;
        }
    }

    // ------------------------------------------------------------------------------- sections

    static int env(String name, int otherwise) {
        String v = System.getenv(name);
        return v == null || v.isEmpty() ? otherwise : Integer.parseInt(v);
    }

    static String describe(Throwable e) {
        StringBuilder s = new StringBuilder();
        for (Throwable t = e; t != null; t = t.getCause()) {
            if (s.length() > 0) s.append(" <- ");
            s.append(t);
            if (t.getCause() == null && t.getStackTrace().length > 0) {
                StackTraceElement[] trace = t.getStackTrace();
                for (int i = 0; i < Math.min(6, trace.length); ++i) s.append("\n      at ").append(trace[i]);
            }
        }
        return s.toString();
    }

    /**
     * The tile of 2x2 chunks around a chunk of every biome of the overworld, decorated; and of all the
     * features that ran, which read or wrote where src/worldgen has to have a rule.
     */
    static JsonObject census(RegistryAccess registries) throws Exception {
        Stand stand = new Stand(WorldgenRef.world(registries, WorldgenRef.SEED_A));
        Map<String, List<int[]>> every = WorldgenGolden.chunksOfEveryBiome(stand.maker);
        int sites = env("WORLDGEN_SITES", Integer.MAX_VALUE), per = env("WORLDGEN_TILES", 1);
        JsonArray failed = new JsonArray();
        long t0 = System.nanoTime();
        int n = 0;
        for (Map.Entry<String, List<int[]>> e : every.entrySet()) {
            if (n++ >= sites) break;
            for (int k = 0; k < Math.min(per, e.getValue().size()); ++k) {
                int tx = e.getValue().get(k)[0] & ~1, tz = e.getValue().get(k)[1] & ~1;
                for (int i = 0; i < 4; ++i) {
                    int cx = tx + (i & 1), cz = tz + (i >> 1);
                    ProtoChunk at = stand.chunks.get(Stand.key(cx, cz));
                    if (at != null && at.getPersistedStatus().isOrAfter(ChunkStatus.FEATURES)) continue;
                    try {
                        stand.decorate(cx, cz);
                    } catch (Exception | LinkageError x) {
                        String what = e.getKey() + " chunk " + cx + ", " + cz + ": " + describe(x);
                        System.err.println("worldgen: census: " + what);
                        failed.add(what);
                        if (failed.size() >= 12) throw new IllegalStateException("too much fails to go on");
                    }
                }
            }
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.addProperty("seed", stand.world.seed());
        root.addProperty("decorated", stand.decorated);
        root.addProperty("terrain", stand.terrains);
        root.addProperty("entities", stand.entities);
        root.add("failed", failed);
        JsonObject features = new JsonObject();
        long[] total = new long[KINDS];
        for (Map.Entry<String, long[]> e : stand.census.runs.entrySet()) {
            JsonObject f = new JsonObject();
            f.addProperty("runs", e.getValue()[0]);
            long[] counts = stand.census.counts.get(e.getKey());
            if (counts != null)
                for (int k = 0; k < KINDS; ++k) {
                    if (counts[k] == 0) continue;
                    f.addProperty(KIND[k], counts[k]);
                    f.addProperty(KIND[k] + "_farthest", stand.census.farthest.get(e.getKey())[k]);
                    total[k] += counts[k];
                }
            features.add(e.getKey(), f);
        }
        root.add("features", features);
        StringBuilder line = new StringBuilder("worldgen: census: " + stand.decorated + " chunks decorated (" + stand.terrains + " with terrain) in "
            + (System.nanoTime() - t0) / 1_000_000 + " ms, " + stand.census.runs.size() + " features ran;");
        for (int k = 0; k < KINDS; ++k) line.append(' ').append(KIND[k]).append(' ').append(total[k]).append(k + 1 < KINDS ? "," : "");
        System.err.println(line);
        for (Map.Entry<String, long[]> e : stand.census.counts.entrySet()) {
            StringBuilder of = new StringBuilder("worldgen: census:   " + e.getKey() + " (" + stand.census.runs.getOrDefault(e.getKey(), new long[1])[0] + " runs):");
            for (int k = 0; k < KINDS; ++k)
                if (e.getValue()[k] != 0) of.append(' ').append(KIND[k]).append(' ').append(e.getValue()[k]).append(" (to ").append(stand.census.farthest.get(e.getKey())[k]).append(')');
            System.err.println(of);
        }
        return root;
    }

    // ------------------------------------------------------------------------------- replay

    static final Heightmap.Types[] KEPT =
        {Heightmap.Types.WORLD_SURFACE, Heightmap.Types.OCEAN_FLOOR, Heightmap.Types.MOTION_BLOCKING, Heightmap.Types.MOTION_BLOCKING_NO_LEAVES};

    static <T> void ticks(WorldgenGolden.Out o, Object container, net.minecraft.core.Registry<T> registry) {
        // (what SerializableChunkData saves of a proto chunk: ProtoChunkTicks.pack - the ticks in the order they were asked for)
        @SuppressWarnings("unchecked")
        List<net.minecraft.world.ticks.SavedTick<T>> ticks = ((net.minecraft.world.ticks.ProtoChunkTicks<T>) container).pack(0L);
        o.i32(ticks.size());
        for (net.minecraft.world.ticks.SavedTick<T> tick : ticks) {
            o.str(registry.getKey(tick.type()).toString());
            o.i32(tick.pos().getX());
            o.i32(tick.pos().getY());
            o.i32(tick.pos().getZ());
            o.i32(tick.delay());
            o.i32(tick.priority().getValue());
        }
    }

    /** A chunk as a server would save it before it is a chunk of the world: everything a decoration can have changed. */
    static void chunk(WorldgenGolden.Out o, Stand stand, ProtoChunk chunk) throws Exception {
        o.i32(chunk.getPos().x());
        o.i32(chunk.getPos().z());
        o.u8(chunk.getPersistedStatus().isOrAfter(ChunkStatus.FEATURES) ? 1 : 0);
        int sections = chunk.getSectionsCount();
        byte[] blocks = new byte[sections * 4096 * 2];
        int at = 0;
        for (int s = 0; s < sections; ++s) {
            net.minecraft.world.level.chunk.LevelChunkSection section = chunk.getSection(s);
            for (int y = 0; y < 16; ++y)
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x) {
                        int id = net.minecraft.world.level.block.Block.getId(section.getBlockState(x, y, z));
                        blocks[at++] = (byte) id;
                        blocks[at++] = (byte) (id >>> 8);
                    }
        }
        java.util.zip.Deflater deflater = new java.util.zip.Deflater(1);
        deflater.setInput(blocks);
        deflater.finish();
        byte[] packed = new byte[blocks.length + 64];
        int n = deflater.deflate(packed);
        deflater.end();
        o.i32(n);
        for (int i = 0; i < n; ++i) o.u8(packed[i]);
        for (Heightmap.Types type : KEPT) {
            Heightmap map = chunk.getOrCreateHeightmapUnprimed(type);
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) o.i32(map.getFirstAvailable(x, z));
        }
        it.unimi.dsi.fastutil.shorts.ShortList[] lists = chunk.getPostProcessing();
        int count = 0;
        for (it.unimi.dsi.fastutil.shorts.ShortList list : lists)
            if (list != null) count += list.size();
        o.i32(count);
        for (int s = 0; s < lists.length; ++s)
            if (lists[s] != null)
                for (short mark : lists[s]) o.i32(s << 12 | (mark & 0xFFF));
        ticks(o, chunk.getBlockTicks(), net.minecraft.core.registries.BuiltInRegistries.BLOCK);
        ticks(o, chunk.getFluidTicks(), net.minecraft.core.registries.BuiltInRegistries.FLUID);
        List<BlockPos> entities = new ArrayList<>(chunk.getBlockEntitiesPos());
        o.i32(entities.size());
        for (BlockPos pos : entities) {
            java.io.ByteArrayOutputStream bytes = new java.io.ByteArrayOutputStream();
            net.minecraft.nbt.NbtIo.write(chunk.getBlockEntityNbtForSaving(pos, stand.world.registries()), new java.io.DataOutputStream(bytes));
            o.i32(bytes.size());
            for (byte b : bytes.toByteArray()) o.u8(b);
        }
        List<int[]> tainted = stand.tainted.getOrDefault(Stand.key(chunk.getPos().x(), chunk.getPos().z()), List.of());
        o.i32(tainted.size());
        for (int[] pos : tainted)
            for (int v : pos) o.i32(v);
    }

    /**
     * replay.txt of the output directory - "seed N", then a line a step: "T x z" (the chunk's terrain is
     * made), "F x z" (it is decorated), "L x z" (the server had begun to light it before the next
     * decoration was over) - run as it stands; every chunk that has its terrain then written to replay.bin.
     */
    static JsonObject replay(RegistryAccess registries) throws Exception {
        List<String> lines = Files.readAllLines(WorldgenRef.out.resolve("replay.txt"));
        long seed = Long.parseLong(lines.get(0).substring(5).trim());
        Stand stand = new Stand(WorldgenRef.world(registries, seed));
        stand.live = true;
        if (env("WORLDGEN_LIGHT_LOG", 0) != 0) stand.asked = new ArrayList<>();
        long t0 = System.nanoTime();
        for (String line : lines.subList(1, lines.size())) {
            if (line.isBlank()) continue;
            String[] part = line.trim().split(" ");
            int x = Integer.parseInt(part[1]), z = Integer.parseInt(part[2]);
            switch (part[0]) {
                case "T" -> stand.terrain(x, z);
                case "F" -> stand.decorate(x, z);
                case "L" -> stand.lighting.add(Stand.key(x, z));
                default -> throw new IllegalStateException("replay.txt: " + line);
            }
        }
        WorldgenGolden.Out o = new WorldgenGolden.Out();
        o.magic("WGREPL01");
        o.i64(seed);
        List<ProtoChunk> made = new ArrayList<>();
        for (ProtoChunk chunk : stand.chunks.values())
            if (chunk.getPersistedStatus().isOrAfter(ChunkStatus.TERRAIN)) made.add(chunk);
        made.sort(java.util.Comparator.<ProtoChunk>comparingInt(c -> c.getPos().x()).thenComparingInt(c -> c.getPos().z()));
        o.i32(made.size());
        for (ProtoChunk chunk : made) chunk(o, stand, chunk);
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.addProperty("seed", seed);
        root.addProperty("decorated", stand.decorated);
        root.addProperty("terrain", stand.terrains);
        root.addProperty("bytes", o.save(WorldgenRef.out.resolve("replay.bin")));
        if (stand.asked != null) Files.write(WorldgenRef.out.resolve("replay.light.txt"), stand.asked);
        // What the order alone does not decide: reads of chunks further off than a decoration writes
        // (they are whatever they happened to be on the server), light asked of a chunk that was lit.
        JsonObject far = new JsonObject();
        for (Map.Entry<String, long[]> e : stand.census.counts.entrySet())
            for (int k = 0; k < RAW_LIGHT; ++k)
                if (e.getValue()[k] != 0) far.addProperty(e.getKey() + "." + KIND[k], e.getValue()[k]);
        root.add("far", far);
        int tainted = 0;
        for (List<int[]> of : stand.tainted.values()) tainted += of.size();
        root.addProperty("tainted", tainted);
        System.err.println("worldgen: replay: " + stand.decorated + " chunks decorated, " + stand.terrains + " with terrain, in " + (System.nanoTime() - t0) / 1_000_000
            + " ms; " + far.size() + " kinds of far access, " + tainted + " blocks that asked for light beside a chunk the server was lighting");
        return root;
    }

    /** Which sections of WorldgenRef want the game loaded with its resources kept. */
    static final List<String> SECTIONS = List.of("census", "replay");
}
