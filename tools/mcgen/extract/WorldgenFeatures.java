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

    /** A section that says what is set in it: every block a feature writes ends here, an ore's too. */
    static final class LoggedSection extends net.minecraft.world.level.chunk.LevelChunkSection {
        final Stand stand;
        final int x0, y0, z0;

        LoggedSection(Stand stand, int x0, int y0, int z0) {
            super(stand.maker.containers);
            this.stand = stand;
            this.x0 = x0;
            this.y0 = y0;
            this.z0 = z0;
        }

        @Override
        public BlockState setBlockState(int x, int y, int z, BlockState state, boolean checkThreading) {
            if (stand.logging) {
                long h = stand.writes;
                for (int v : new int[] {x0 + x, y0 + y, z0 + z, net.minecraft.world.level.block.Block.getId(state)}) h = WorldgenGolden.fnv(h, v);
                stand.writes = h;
                if (stand.tracing) System.err.println("TRACE " + (x0 + x) + " " + (y0 + y) + " " + (z0 + z) + " " + net.minecraft.world.level.block.Block.getId(state));
            }
            return super.setBlockState(x, y, z, state, checkThreading);
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
        /** While `logging`: every block set in a chunk of this stand - where, and to what - folded into `writes`. */
        boolean logging;
        long writes;
        /** ...and each of them said on stderr (WORLDGEN_TRACE: for whoever looks for where two decorations part). */
        boolean tracing;

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

        /** A chunk with nothing in it, of sections that say what is set in them. */
        ProtoChunk empty(int cx, int cz) {
            var noise = world.settings().noiseSettings();
            var height = net.minecraft.world.level.LevelHeightAccessor.create(noise.minY(), noise.height());
            net.minecraft.world.level.chunk.LevelChunkSection[] sections = new net.minecraft.world.level.chunk.LevelChunkSection[height.getSectionsCount()];
            for (int i = 0; i < sections.length; ++i) sections[i] = new LoggedSection(this, cx * 16, noise.minY() + i * 16, cz * 16);
            return new ProtoChunk(new ChunkPos(cx, cz), net.minecraft.world.level.chunk.UpgradeData.EMPTY, sections,
                new net.minecraft.world.ticks.ProtoChunkTicks<>(), new net.minecraft.world.ticks.ProtoChunkTicks<>(), height, maker.containers, null);
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
                ProtoChunk made = empty(cx, cz);
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
                ProtoChunk empty = empty(x, z);
                empty.setPersistedStatus(ChunkStatus.STRUCTURE_STARTS);
                return empty;
            });
        }

        /** The region a decoration of the chunk works in: the eight around it must have their terrain. */
        Region regionAround(int cx, int cz) throws Exception {
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    if (!live) terrain(cx + dx, cz + dz);
                    ProtoChunk near = chunks.get(key(cx + dx, cz + dz));
                    if (near == null || !near.getPersistedStatus().isOrAfter(ChunkStatus.TERRAIN))
                        throw new IllegalStateException("chunk " + cx + ", " + cz + " is decorated before " + (cx + dx) + ", " + (cz + dz) + " has its terrain");
                }
            StaticCache2D<GenerationChunkHolder> cache = StaticCache2D.create(cx, cz, 8, (x, z) -> new Slot(new ChunkPos(x, z), given(cx, cz, x, z)));
            return new Region(this, cache, chunks.get(key(cx, cz)));
        }

        /** ChunkStatusTasks.generateFeatures, and the status the step that ran it then gives the chunk. */
        void decorate(int cx, int cz) throws Exception {
            Region made = regionAround(cx, cz);
            ProtoChunk center = chunks.get(key(cx, cz));
            if (center.getPersistedStatus().isOrAfter(ChunkStatus.FEATURES)) throw new IllegalStateException("chunk " + cx + ", " + cz + " is decorated twice");
            region = made;
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

    // ------------------------------------------------------------------------------- golden

    static long fnv(long h, byte[] bytes) {
        for (byte b : bytes) h = (h ^ (b & 0xFF)) * 0x100000001b3L;
        return h;
    }

    /** JSON as Python's json.dumps(sort_keys=True, separators=(",", ":")) writes it - the name gen_features.py has for a predicate. */
    static void canon(com.google.gson.JsonElement e, StringBuilder s) {
        if (e.isJsonObject()) {
            s.append('{');
            boolean first = true;
            for (String key : new java.util.TreeSet<>(e.getAsJsonObject().keySet())) {
                if (!first) s.append(',');
                first = false;
                s.append(new com.google.gson.JsonPrimitive(key)).append(':');
                canon(e.getAsJsonObject().get(key), s);
            }
            s.append('}');
        } else if (e.isJsonArray()) {
            s.append('[');
            for (int i = 0; i < e.getAsJsonArray().size(); ++i) {
                if (i > 0) s.append(',');
                canon(e.getAsJsonArray().get(i), s);
            }
            s.append(']');
        } else {
            s.append(e);
        }
    }

    /** The source vanilla draws a chunk's decoration from: WorldgenRandom over a Xoroshiro source. */
    static net.minecraft.world.level.levelgen.WorldgenRandom source(long seed) {
        return new net.minecraft.world.level.levelgen.WorldgenRandom(new net.minecraft.world.level.levelgen.XoroshiroRandomSource(seed));
    }

    // IntProvider, by src/worldgen's IntOp: {op, a, b, c} and two floats.
    static final Object[][] INTS = {
        {0, 7, 0, 0, 0f, 0f}, {1, 0, 0, 0, 0f, 0f}, {1, -5, 7, 0, 0f, 0f}, {1, 1, 64, 0, 0f, 0f}, {1, 104, 157, 0, 0f, 0f},
        {2, -7, 7, 0, 0f, 0f}, {2, -3, 3, 0, 0f, 0f}, {2, 0, 10, 10, 0f, 0f}, {2, 2, 12, 4, 0f, 0f}, {2, -6, 10, 3, 0f, 0f}, {2, 0, 5, 0, 0f, 0f},
        {5, 0, 16, 0, 8f, 4f}, {5, -3, 3, 0, 0f, 1f}, {5, 1, 4, 0, 2.5f, 0.7f},
        {6, 0, 10, 0, 0f, 0f}, {6, 3, 3, 0, 0f, 0f}, {7, 1, 17, 0, 0f, 0f}, {7, 0, 0, 0, 0f, 0f},
    };

    static net.minecraft.util.valueproviders.IntProvider intProvider(Object[] spec) {
        int a = (Integer) spec[1], b = (Integer) spec[2], c = (Integer) spec[3];
        return switch ((Integer) spec[0]) {
            case 0 -> net.minecraft.util.valueproviders.ConstantInt.of(a);
            case 1 -> net.minecraft.util.valueproviders.UniformInt.of(a, b);
            case 2 -> net.minecraft.util.valueproviders.TrapezoidInt.of(a, b, c);
            case 5 -> net.minecraft.util.valueproviders.ClampedNormalInt.of((Float) spec[4], (Float) spec[5], a, b);
            case 6 -> net.minecraft.util.valueproviders.BiasedToBottomInt.of(a, b);
            case 7 -> net.minecraft.util.valueproviders.VeryBiasedToBottomInt.of(a, b);
            default -> throw new IllegalStateException();
        };
    }

    // HeightProvider, by HeightOp: {op, min, max, inner}.
    static final int[][] HEIGHTS = {
        {0, 17, 0, 0}, {1, -64, 320, 0}, {1, 10, 10, 0}, {1, 20, 5, 0},
        {2, -64, 64, 0}, {2, 0, 100, 100}, {2, 0, 100, 200}, {2, -24, 56, 12}, {2, 30, 20, 0},
        {3, 0, 50, 1}, {3, -64, -40, 8}, {3, 5, 5, 3}, {4, -64, 0, 1}, {4, 0, 96, 24},
    };

    static net.minecraft.world.level.levelgen.heightproviders.HeightProvider heightProvider(int[] spec) {
        var min = net.minecraft.world.level.levelgen.VerticalAnchor.absolute(spec[1]);
        var max = net.minecraft.world.level.levelgen.VerticalAnchor.absolute(spec[2]);
        return switch (spec[0]) {
            case 0 -> net.minecraft.world.level.levelgen.heightproviders.ConstantHeight.of(min);
            case 1 -> net.minecraft.world.level.levelgen.heightproviders.UniformHeight.of(min, max);
            case 2 -> net.minecraft.world.level.levelgen.heightproviders.TrapezoidHeight.of(min, max, spec[3]);
            case 3 -> net.minecraft.world.level.levelgen.heightproviders.BiasedToBottomHeight.of(min, max, spec[3]);
            case 4 -> net.minecraft.world.level.levelgen.heightproviders.VeryBiasedToBottomHeight.of(min, max, spec[3]);
            default -> throw new IllegalStateException();
        };
    }

    /** A feature that only says where it was asked to be: what a placed feature's placements come to. */
    static final class Recorder implements net.minecraft.world.level.levelgen.feature.Feature {
        final List<BlockPos> at = new ArrayList<>();

        @Override
        public com.mojang.serialization.MapCodec<? extends net.minecraft.world.level.levelgen.feature.Feature> codec() {
            return null;
        }

        @Override
        public boolean place(net.minecraft.world.level.WorldGenLevel level, ChunkGenerator generator, net.minecraft.util.RandomSource random, BlockPos pos) {
            at.add(pos.immutable());
            return true;
        }
    }

    /**
     * features.bin, "WGFEAT01":
     *
     *   numbers     IntProviders and HeightProviders made of numbers written here: for each, and for a few
     *               seeds, what a source gives through them, and the long the source gives next
     *   sets        every predicate of the jar's features that asks nothing but what block state is at a
     *               place: the hash of its name (the JSON, keys in order) and of the set of the states
     *               it holds for, a bit a state
     *   decorations whole decorations with the features of one kind alone, kind by kind (decorations())
     *   attempts    a kind of feature placed again and again in one place (attempts())
     *   survive     the rules of canSurvive held to the method in worlds drawn at random (WorldgenSurvive.golden)
     *   shapes      the rules of updateShape held to the method in worlds drawn at random (WorldgenShape.golden)
     *   hash sets   the order of a HashSet of positions, by scripts run from a number (WorldgenHash.golden)
     *   math        Math.sin and Math.cos of every angle a feature can make of a float: all of the answers
     *               as one number, and a few of them written out (WorldgenMath.golden; at the end of the file)
     *   placements  chunks of real terrain around which, between them, every feature of the overworld's
     *               biomes is to be placed: for each feature vanilla would run there, in its order, the
     *               positions its placements come to (a feature that only writes them down stands in its
     *               place), and the long its source gives afterwards
     */
    static int golden(Path file, WorldgenRef.World world) throws Exception {
        WorldgenGolden.Out o = new WorldgenGolden.Out();
        o.magic("WGFEAT01");
        o.i64(world.seed());
        long[] seeds = {1L, -7046029254386353131L, 20261124L};

        o.i32(INTS.length * seeds.length);
        for (Object[] spec : INTS)
            for (long seed : seeds) {
                o.u8((Integer) spec[0]);
                for (int i = 1; i <= 3; ++i) o.i32((Integer) spec[i]);
                o.f32((Float) spec[4]);
                o.f32((Float) spec[5]);
                o.i64(seed);
                var random = source(seed);
                var provider = intProvider(spec);
                for (int i = 0; i < 12; ++i) o.i32(provider.sample(random));
                o.i64(random.nextLong());
            }
        Stand stand = new Stand(world);
        var context = new net.minecraft.world.level.levelgen.WorldGenerationContext(stand.maker.generator, stand.maker.chunk(0, 0));
        o.i32(HEIGHTS.length * seeds.length);
        for (int[] spec : HEIGHTS)
            for (long seed : seeds) {
                o.u8(spec[0]);
                for (int i = 1; i <= 3; ++i) o.i32(spec[i]);
                o.i64(seed);
                var random = source(seed);
                var provider = heightProvider(spec);
                for (int i = 0; i < 12; ++i) o.i32(provider.sample(random, context));
                o.i64(random.nextLong());
            }

        // The sets: a level in which every block is the one state that is asked about.
        BlockState[] asked = new BlockState[1];
        net.minecraft.world.level.LevelAccessor everywhere = (net.minecraft.world.level.LevelAccessor) java.lang.reflect.Proxy.newProxyInstance(
            WorldgenFeatures.class.getClassLoader(), new Class<?>[] {net.minecraft.world.level.LevelAccessor.class}, (proxy, method, args) -> {
                if (method.getName().equals("getBlockState")) return asked[0];
                if (method.getName().equals("getFluidState")) return asked[0].getFluidState();
                // (What a block's shape asks of the world around it - a shulker box its block entity - is
                // answered as for a block that stands alone: the tables are made that way, mcgen.py gamedata.)
                if (method.getDeclaringClass().isInstance(net.minecraft.world.level.EmptyBlockGetter.INSTANCE))
                    return method.invoke(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, args);
                throw new UnsupportedOperationException(method.getName());
            });
        var ops = world.registries().createSerializationContext(com.mojang.serialization.JsonOps.INSTANCE);
        int states = net.minecraft.world.level.block.Block.BLOCK_STATE_REGISTRY.size();
        Map<String, Long> sets = new TreeMap<>();
        java.util.ArrayDeque<com.google.gson.JsonElement> open = new java.util.ArrayDeque<>();
        for (String kind : new String[] {"worldgen/placed_feature", "worldgen/feature", "worldgen/block_state_provider"})
            for (var found : resources.listResources(kind, id -> id.getPath().endsWith(".json")).entrySet())
                try (java.io.Reader reader = found.getValue().openAsReader()) {
                    open.add(com.google.gson.JsonParser.parseReader(reader));
                }
        while (!open.isEmpty()) {
            com.google.gson.JsonElement e = open.poll();
            if (e.isJsonArray()) e.getAsJsonArray().forEach(open::add);
            if (!e.isJsonObject()) continue;
            e.getAsJsonObject().asMap().values().forEach(open::add);
            // (What a tree's trunk and roots grow through, and where its roots are muddy: sets of states the same way.)
            for (String blocks : new String[] {"can_grow_through", "muddy_roots_in", "replaceable", "root_replaceable", "replaceable_blocks", "cannot_replace",
                                               "invalid_blocks", "can_be_placed_on"})
                if (e.getAsJsonObject().has(blocks)) {
                    JsonObject same = new JsonObject();
                    same.addProperty("type", "minecraft:matching_blocks");
                    same.add("blocks", e.getAsJsonObject().get(blocks));
                    open.add(same);
                }
            if (e.getAsJsonObject().has("valid_blocks")) {
                // (A spring's blocks are a set of states to src/worldgen as well: the predicate that names the same blocks.)
                JsonObject same = new JsonObject();
                same.addProperty("type", "minecraft:matching_blocks");
                same.add("blocks", e.getAsJsonObject().get("valid_blocks"));
                open.add(same);
            }
            if (!e.getAsJsonObject().has("type") && !e.getAsJsonObject().has("predicate_type")) continue;
            StringBuilder name = new StringBuilder();
            canon(e, name);
            if (sets.containsKey(name.toString())) continue;
            byte[] bits = new byte[(states + 7) / 8];
            if (e.getAsJsonObject().has("predicate_type")) {
                // A RuleTest (what an ore replaces): of a state alone if the height it is asked at changes nothing.
                var rule = net.minecraft.world.level.levelgen.structure.templatesystem.RuleTest.CODEC.parse(ops, e).result();
                if (rule.isEmpty()) continue;
                boolean ofTheStateAlone = true;
                try {
                    for (int id = 0; id < states && ofTheStateAlone; ++id) {
                        BlockState state = net.minecraft.world.level.block.Block.stateById(id);
                        boolean held = rule.get().test(state, BlockPos.ZERO, null);
                        for (int y : new int[] {-100000, -64, 319, 100000}) ofTheStateAlone &= rule.get().test(state, new BlockPos(0, y, 0), null) == held;
                        if (held) bits[id >> 3] |= (byte) (1 << (id & 7));
                    }
                } catch (NullPointerException drawsANumber) {
                    continue;
                }
                if (ofTheStateAlone) sets.put(name.toString(), fnv(0xcbf29ce484222325L, bits));
                continue;
            }
            var parsed = net.minecraft.world.level.levelgen.blockpredicates.BlockPredicate.CODEC.parse(ops, e).result();
            if (parsed.isEmpty()) continue;
            try {
                for (int id = 0; id < states; ++id) {
                    asked[0] = net.minecraft.world.level.block.Block.stateById(id);
                    if (parsed.get().test(everywhere, BlockPos.ZERO)) bits[id >> 3] |= (byte) (1 << (id & 7));
                }
            } catch (java.lang.reflect.UndeclaredThrowableException | UnsupportedOperationException notOfAStateAlone) {
                continue;
            }
            sets.put(name.toString(), fnv(0xcbf29ce484222325L, bits));
        }
        // ...and what the code of src/worldgen asks of a state by itself, with no predicate of the data behind
        // it: sets like the others, under the names gen_features.py gives them.
        Map<String, java.util.function.Predicate<BlockState>> facts = new java.util.LinkedHashMap<>();
        facts.put("kSetLiquid", BlockState::liquid);
        for (net.minecraft.core.Direction direction : net.minecraft.core.Direction.values())
            facts.put("kSetFaceOccludes " + direction.getName(), state -> (faceOccludes(state) >> direction.ordinal() & 1) != 0);
        facts.put("kSetSnowSurvivesOn", WorldgenFeatures::snowSurvivesOn);
        for (net.minecraft.core.Direction direction : net.minecraft.core.Direction.values())
            facts.put("kSetVineAttaches " + direction.getName(), state -> (vineAttaches(state) >> direction.ordinal() & 1) != 0);
        facts.put("kSet a big mushroom grows through", state -> state.isAir() || state.is(net.minecraft.tags.BlockTags.REPLACEABLE_BY_MUSHROOMS));
        facts.put("kSet a big mushroom has room in", state -> state.isAir() || state.is(net.minecraft.tags.BlockTags.LEAVES));
        facts.put("kSet podzol takes the place of under bamboo", state -> state.is(net.minecraft.tags.BlockTags.BENEATH_BAMBOO_PODZOL_REPLACEABLE));
        // The sets the rules of canSurvive that are written by hand are made of (src/worldgen/survive.hpp).
        facts.put("kSet a cactus has beside it", state -> !(state.isSolid() || state.getFluidState().is(net.minecraft.tags.FluidTags.LAVA)));
        facts.put("kSet a cactus stands on", state -> state.is(net.minecraft.world.level.block.Blocks.CACTUS) || state.is(net.minecraft.tags.BlockTags.SUPPORTS_CACTUS));
        facts.put("kSet that is no liquid", state -> !state.liquid());
        facts.put("kSet a lily pad lies on", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.SUPPORTS_LILY_PAD) || state.is(net.minecraft.tags.BlockTags.SUPPORTS_LILY_PAD));
        facts.put("kSet that holds no fluid", state -> state.getFluidState().is(net.minecraft.world.level.material.Fluids.EMPTY));
        facts.put("kSet a big dripleaf's stem stands on", state -> state.is(net.minecraft.world.level.block.Blocks.BIG_DRIPLEAF_STEM) || state.is(net.minecraft.tags.BlockTags.SUPPORTS_BIG_DRIPLEAF));
        facts.put("kSet a big dripleaf's stem has above it", state -> state.is(net.minecraft.world.level.block.Blocks.BIG_DRIPLEAF_STEM) || state.is(net.minecraft.world.level.block.Blocks.BIG_DRIPLEAF));
        facts.put("kSet a spore blossom hangs from", state -> {
            asked[0] = state;
            return net.minecraft.world.level.block.Block.canSupportCenter(everywhere, BlockPos.ZERO, net.minecraft.core.Direction.DOWN);
        });
        facts.put("kSet that holds no water", state -> !state.getFluidState().is(net.minecraft.tags.FluidTags.WATER));
        facts.put("kSet tall seagrass stands on", state -> {
            asked[0] = state;
            return state.isFaceSturdy(everywhere, BlockPos.ZERO, net.minecraft.core.Direction.UP) && !state.is(net.minecraft.tags.BlockTags.CANNOT_SUPPORT_SEAGRASS);
        });
        facts.put("kSet full of water", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.WATER) && state.getFluidState().isFull());
        facts.put("kSet a small dripleaf stands on", state -> state.is(net.minecraft.tags.BlockTags.SUPPORTS_SMALL_DRIPLEAF));
        facts.put("kSet that holds a source of water", state -> state.getFluidState().isSourceOfType(net.minecraft.world.level.material.Fluids.WATER));
        facts.put("kSet plants stand on", state -> state.is(net.minecraft.tags.BlockTags.SUPPORTS_VEGETATION));
        facts.put("kSet of sugar cane", state -> state.is(net.minecraft.world.level.block.Blocks.SUGAR_CANE));
        facts.put("kSet sugar cane stands on", state -> state.is(net.minecraft.tags.BlockTags.SUPPORTS_SUGAR_CANE));
        facts.put("kSet sugar cane has beside its ground", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.SUPPORTS_SUGAR_CANE_ADJACENTLY)
            || state.is(net.minecraft.tags.BlockTags.SUPPORTS_SUGAR_CANE_ADJACENTLY));
        facts.put("kSet a mushroom needs no dark on", state -> state.is(net.minecraft.tags.BlockTags.OVERRIDES_MUSHROOM_LIGHT_REQUIREMENT));
        facts.put("kSet of whole blocks that let no light through", BlockState::isSolidRender);
        // Trees, and the block updates at their edges.
        facts.put("kSet a tree grows through", state -> state.isAir() || state.is(net.minecraft.tags.BlockTags.REPLACEABLE_BY_TREES));
        facts.put("kSet of logs", state -> state.is(net.minecraft.tags.BlockTags.LOGS));
        facts.put("kSet that keeps leaves from decaying", state -> state.is(net.minecraft.tags.BlockTags.PREVENTS_NEARBY_LEAF_DECAY));
        facts.put("kSet that makes the ground under it snowy", state -> state.is(net.minecraft.tags.BlockTags.SNOW));
        facts.put("kSet that gives way to a block placed", BlockState::canBeReplaced);
        facts.put("kSet that holds water", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.WATER));
        facts.put("kSet whose fluid is a source", state -> state.getFluidState().isSource());
        facts.put("kSet a column of bubbles rises from", state -> state.is(net.minecraft.tags.BlockTags.ENABLES_BUBBLE_COLUMN_DRAG_DOWN)
            || state.is(net.minecraft.tags.BlockTags.ENABLES_BUBBLE_COLUMN_PUSH_UP));
        facts.put("kSet a column of bubbles takes the place of", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.BUBBLE_COLUMN_CAN_OCCUPY)
            && state.getFluidState().isSource() && state.getFluidState().isFull());
        facts.put("kSet of pale oak logs", state -> state.is(net.minecraft.tags.BlockTags.PALE_OAK_LOGS));
        for (net.minecraft.core.Direction direction : net.minecraft.core.Direction.values())
            facts.put("kSetSturdy " + direction.getName(), state -> state.isFaceSturdy(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO, direction));
        // Caves.
        facts.put("kSet whose collision shape is a whole block", state -> state.isCollisionShapeFullBlock(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO));
        facts.put("kSet sculk takes the place of", state -> state.is(net.minecraft.tags.BlockTags.SCULK_REPLACEABLE));
        facts.put("kSet sculk takes the place of as a world is made", state -> state.is(net.minecraft.tags.BlockTags.SCULK_REPLACEABLE_WORLD_GEN));
        facts.put("kSet that keeps sculk from growing things", state -> state.is(net.minecraft.tags.BlockTags.SCULK_GROWTH_INHIBITORS));
        facts.put("kSet of fire", state -> state.is(net.minecraft.tags.BlockTags.FIRE));
        facts.put("kSet that no feature takes the place of", state -> state.is(net.minecraft.tags.BlockTags.FEATURES_CANNOT_REPLACE));
        facts.put("kSet of the overworld's stone", state -> state.is(net.minecraft.tags.BlockTags.BASE_STONE_OVERWORLD));
        facts.put("kSet whose fluid fills the block", state -> state.getFluidState().isFull());
        facts.put("kSet that holds lava", state -> state.getFluidState().is(net.minecraft.tags.FluidTags.LAVA));
        facts.put("kSet that is solid", BlockState::isSolid);
        facts.put("kSet that holds water and nothing else of a fluid", state -> state.getFluidState().isEmpty()
            || state.getFluidState().is(net.minecraft.world.level.material.Fluids.WATER));
        for (var fact : facts.entrySet()) {
            byte[] bits = new byte[(states + 7) / 8];
            for (int id = 0; id < states; ++id)
                if (fact.getValue().test(net.minecraft.world.level.block.Block.stateById(id))) bits[id >> 3] |= (byte) (1 << (id & 7));
            sets.put('"' + fact.getKey() + '"', fnv(0xcbf29ce484222325L, bits));
        }
        // ...and the sets the jar itself made of canSurvive (survive.json): as they lie there, by their number.
        java.nio.file.Path survive = WorldgenRef.out.resolve("survive.json");
        if (Files.exists(survive)) {
            var made = com.google.gson.JsonParser.parseString(Files.readString(survive)).getAsJsonObject().getAsJsonArray("sets");
            for (int i = 0; i < made.size(); ++i) sets.put("\"survives " + i + "\"", fnv(0xcbf29ce484222325L, java.util.HexFormat.of().parseHex(made.get(i).getAsString())));
        }
        o.i32(sets.size());
        for (Map.Entry<String, Long> e : sets.entrySet()) {
            o.i64(fnv(0xcbf29ce484222325L, e.getKey().getBytes(java.nio.charset.StandardCharsets.UTF_8)));
            o.i64(e.getValue());
        }

        // The placements. A chunk for every biome that has a feature no chunk before it brought.
        Field field = ChunkGenerator.class.getDeclaredField("featuresPerStep");
        field.setAccessible(true);
        @SuppressWarnings("unchecked")
        List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps =
            ((Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>) field.get(stand.maker.generator)).get();
        Map<String, List<int[]>> every = WorldgenGolden.chunksOfEveryBiome(stand.maker);
        var biomes = world.registries().lookupOrThrow(Registries.BIOME);
        java.util.Set<Long> brought = new java.util.HashSet<>();
        List<int[]> sites = new ArrayList<>();
        for (Map.Entry<String, List<int[]>> e : every.entrySet()) {
            var settings = stand.maker.generator.getBiomeGenerationSettings(biomes.getOrThrow(net.minecraft.resources.ResourceKey.create(Registries.BIOME,
                net.minecraft.resources.Identifier.parse(e.getKey()))));
            boolean news = false;
            for (int s = 0; s < settings.features().size(); ++s)
                for (var feature : settings.features().get(s)) news |= !brought.contains((long) s << 32 | steps.get(s).indexMapping().applyAsInt(feature.value()));
            if (!news) continue;
            // What runs around that chunk is the features of every biome that is somewhere in the nine.
            int cx = e.getValue().getFirst()[0], cz = e.getValue().getFirst()[1];
            sites.add(e.getValue().getFirst());
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz) {
                    java.util.Set<net.minecraft.core.Holder<net.minecraft.world.level.biome.Biome>> there = new java.util.HashSet<>();
                    for (var section : stand.terrain(cx + dx, cz + dz).getSections()) section.getBiomes().getAll(there::add);
                    there.retainAll(stand.maker.biomeSource.possibleBiomes());
                    for (var biome : there) {
                        var features = stand.maker.generator.getBiomeGenerationSettings(biome).features();
                        for (int s = 0; s < features.size(); ++s)
                            for (var feature : features.get(s)) brought.add((long) s << 32 | steps.get(s).indexMapping().applyAsInt(feature.value()));
                    }
                }
        }
        o.i32(sites.size());
        Recorder recorder = new Recorder();
        int placed = 0, unrecorded = 0, positions = 0;
        for (int n = 0; n < sites.size(); ++n) {
            int cx = sites.get(n)[0], cz = sites.get(n)[1];
            boolean whole = n < 2;
            Region region = stand.regionAround(cx, cz);
            ProtoChunk center = stand.chunks.get(Stand.key(cx, cz));
            stand.region = region;
            // ChunkGenerator.applyBiomeDecoration, down to where a feature is placed.
            BlockPos origin = net.minecraft.core.SectionPos.of(center.getPos(), region.getMinSectionY()).origin();
            var random = source(0L);
            long decorationSeed = random.setDecorationSeed(region.getSeed(), origin.getX(), origin.getZ());
            java.util.Set<net.minecraft.core.Holder<net.minecraft.world.level.biome.Biome>> possible = new java.util.HashSet<>();
            for (int dx = -1; dx <= 1; ++dx)
                for (int dz = -1; dz <= 1; ++dz)
                    for (var section : region.getChunk(cx + dx, cz + dz).getSections()) section.getBiomes().getAll(possible::add);
            possible.retainAll(stand.maker.biomeSource.possibleBiomes());
            var placer = new net.minecraft.world.level.levelgen.placement.FeaturePlacer(region, stand.maker.generator);
            List<int[]> ran = new ArrayList<>();
            for (int s = 0; s < steps.size(); ++s) {
                java.util.TreeSet<Integer> indices = new java.util.TreeSet<>();
                for (var biome : possible) {
                    var features = stand.maker.generator.getBiomeGenerationSettings(biome).features();
                    if (s >= features.size()) continue;
                    for (var feature : features.get(s)) indices.add(steps.get(s).indexMapping().applyAsInt(feature.value()));
                }
                for (int index : indices) ran.add(new int[] {s, index});
            }
            o.i32(cx);
            o.i32(cz);
            o.u8(whole ? 1 : 0);
            o.i32(ran.size());
            for (int[] at : ran) {
                var feature = steps.get(at[0]).features().get(at[1]);
                random.setFeatureSeed(decorationSeed, at[1], at[0]);
                o.u8(at[0]);
                o.i32(at[1]);
                if (!(feature.feature() instanceof net.minecraft.core.Holder.Reference<?> reference)) {
                    o.i32(-1);  // (its feature is written out in place: no other can be put there)
                    ++unrecorded;
                    continue;
                }
                Object real = reference.value();
                recorder.at.clear();
                set(reference, "value", recorder);
                try {
                    placer.placeWithBiomeCheck(feature, random, origin);
                } finally {
                    set(reference, "value", real);
                }
                o.i32(recorder.at.size());
                long hash = 0xcbf29ce484222325L;
                for (BlockPos pos : recorder.at) {
                    hash = WorldgenGolden.fnv(WorldgenGolden.fnv(WorldgenGolden.fnv(hash, pos.getX()), pos.getY()), pos.getZ());
                    if (whole) {
                        o.i32(pos.getX());
                        o.i32(pos.getY());
                        o.i32(pos.getZ());
                    }
                }
                o.i64(hash);
                o.i64(random.nextLong());
                ++placed;
                positions += recorder.at.size();
            }
            stand.region = null;
        }
        System.err.println("worldgen: golden features: " + sets.size() + " sets of block states; " + sites.size() + " chunks, " + placed
            + " placed features in them come to " + positions + " positions (" + unrecorded + " more have a feature of their own written out)");
        decorations(o, world, steps, every);
        attempts(o, world, steps, every);
        WorldgenSurvive.golden(o);
        WorldgenShape.golden(o);
        WorldgenHash.golden(o);
        WorldgenMath.golden(o);
        return o.save(file);
    }

    /** The kinds of feature that are held to vanilla one kind at a time: a name, the class, and biomes to decorate chunks of. */
    static final String[][] KINDS_OF_FEATURE = {
        {"ore", "net.minecraft.world.level.levelgen.feature.OreFeature", "minecraft:windswept_hills", "minecraft:badlands", "minecraft:dripstone_caves",
            "minecraft:lush_caves"},
        // No class: every feature src/worldgen places as its tables stand (ported_features.txt, which
        // `mcgen.py generate` writes) - a chunk's whole decoration as the server's generator makes it.
        {"ported", null, "minecraft:windswept_hills", "minecraft:badlands", "minecraft:dripstone_caves", "minecraft:lush_caves", "minecraft:plains",
            "minecraft:ocean", "minecraft:forest", "minecraft:jungle", "minecraft:old_growth_spruce_taiga", "minecraft:swamp", "minecraft:dark_forest",
            "minecraft:mangrove_swamp", "minecraft:savanna", "minecraft:cherry_grove", "minecraft:dappled_forest", "minecraft:bamboo_jungle",
            "minecraft:deep_dark", "minecraft:pale_garden", "minecraft:sulfur_caves"},
    };

    static void chunkState(WorldgenGolden.Out o, ProtoChunk chunk) {
        long blocks = 0xcbf29ce484222325L;
        for (int s = 0; s < chunk.getSectionsCount(); ++s) {
            var section = chunk.getSection(s);
            for (int y = 0; y < 16; ++y)
                for (int z = 0; z < 16; ++z)
                    for (int x = 0; x < 16; ++x) blocks = WorldgenGolden.fnv(blocks, net.minecraft.world.level.block.Block.getId(section.getBlockState(x, y, z)));
        }
        o.i64(blocks);
        long heights = 0xcbf29ce484222325L;
        for (Heightmap.Types type : KEPT) {
            Heightmap map = chunk.getOrCreateHeightmapUnprimed(type);
            for (int z = 0; z < 16; ++z)
                for (int x = 0; x < 16; ++x) heights = WorldgenGolden.fnv(heights, map.getFirstAvailable(x, z));
        }
        o.i64(heights);
        long marks = 0xcbf29ce484222325L;
        int count = 0;
        it.unimi.dsi.fastutil.shorts.ShortList[] lists = chunk.getPostProcessing();
        for (int s = 0; s < lists.length; ++s)
            if (lists[s] != null)
                for (short mark : lists[s]) {
                    marks = WorldgenGolden.fnv(marks, s << 12 | (mark & 0xFFF));
                    ++count;
                }
        o.i32(count);
        o.i64(marks);
        o.i32(((net.minecraft.world.ticks.ProtoChunkTicks<?>) chunk.getBlockTicks()).pack(0L).size());
        o.i32(((net.minecraft.world.ticks.ProtoChunkTicks<?>) chunk.getFluidTicks()).pack(0L).size());
        o.i32(chunk.getBlockEntitiesPos().size());
    }

    /**
     * Whole decorations, a kind of feature at a time: areas of 4 x 4 chunks of terrain whose inner four
     * are decorated one after the other - (1, 1), (2, 1), (1, 2), (2, 2), the order of src/worldgen - with
     * the features of that kind alone. Of every feature that ran: what it wrote, folded into one number,
     * and the long its source gave afterwards; then what each of the sixteen chunks has become.
     */
    static void decorations(WorldgenGolden.Out o, WorldgenRef.World world, List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps,
                            Map<String, List<int[]>> every) throws Exception {
        o.i32(KINDS_OF_FEATURE.length);
        var names = world.registries().lookupOrThrow(net.minecraft.core.registries.Registries.PLACED_FEATURE);
        for (String[] kind : KINDS_OF_FEATURE) {
            java.util.function.Predicate<net.minecraft.world.level.levelgen.placement.PlacedFeature> wanted;
            if (kind[1] != null) {
                Class<?> type = Class.forName(kind[1]);
                wanted = placed -> type.isInstance(placed.feature().value());
            } else {
                java.nio.file.Path list = WorldgenRef.out.resolve("ported_features.txt");
                if (!Files.exists(list)) {
                    WorldgenRef.problem("no " + list + ": `mcgen.py generate` writes which features src/worldgen places");
                    return;
                }
                java.util.Set<String> ported = new java.util.HashSet<>(Files.readAllLines(list));
                wanted = placed -> {
                    var key = names.getKey(placed);
                    return key != null && ported.contains(key.toString());
                };
            }
            Stand stand = new Stand(world);
            o.str(kind[0]);
            o.i32(kind.length - 2);
            int ran = 0;
            for (int site = 2; site < kind.length; ++site) {
                int x0 = (every.get(kind[site]).getFirst()[0] & ~3) + 8, z0 = (every.get(kind[site]).getFirst()[1] & ~3) + 8;  // (apart from what else was made there)
                o.i32(x0);
                o.i32(z0);
                for (int i = 0; i < 4; ++i) {
                    int cx = x0 + 1 + (i & 1), cz = z0 + 1 + (i >> 1);
                    Region region = stand.regionAround(cx, cz);
                    ProtoChunk center = stand.chunks.get(Stand.key(cx, cz));
                    stand.region = region;
                    BlockPos origin = net.minecraft.core.SectionPos.of(center.getPos(), region.getMinSectionY()).origin();
                    var random = source(0L);
                    long decorationSeed = random.setDecorationSeed(region.getSeed(), origin.getX(), origin.getZ());
                    java.util.Set<net.minecraft.core.Holder<net.minecraft.world.level.biome.Biome>> possible = new java.util.HashSet<>();
                    for (int dx = -1; dx <= 1; ++dx)
                        for (int dz = -1; dz <= 1; ++dz)
                            for (var section : region.getChunk(cx + dx, cz + dz).getSections()) section.getBiomes().getAll(possible::add);
                    possible.retainAll(stand.maker.biomeSource.possibleBiomes());
                    var placer = new net.minecraft.world.level.levelgen.placement.FeaturePlacer(region, stand.maker.generator);
                    List<long[]> made = new ArrayList<>();
                    for (int s = 0; s < steps.size(); ++s) {
                        java.util.TreeSet<Integer> indices = new java.util.TreeSet<>();
                        for (var biome : possible) {
                            var features = stand.maker.generator.getBiomeGenerationSettings(biome).features();
                            if (s >= features.size()) continue;
                            for (var feature : features.get(s)) indices.add(steps.get(s).indexMapping().applyAsInt(feature.value()));
                        }
                        for (int index : indices) {
                            var feature = steps.get(s).features().get(index);
                            if (!wanted.test(feature)) continue;
                            random.setFeatureSeed(decorationSeed, index, s);
                            stand.writes = 0xcbf29ce484222325L;
                            stand.logging = true;
                            placer.placeWithBiomeCheck(feature, random, origin);
                            stand.logging = false;
                            made.add(new long[] {s, index, stand.writes, random.nextLong()});
                        }
                    }
                    stand.region = null;
                    center.setPersistedStatus(ChunkStatus.FEATURES);
                    o.i32(made.size());
                    for (long[] of : made) {
                        o.u8((int) of[0]);
                        o.i32((int) of[1]);
                        o.i64(of[2]);
                        o.i64(of[3]);
                    }
                    ran += made.size();
                }
                for (int dz = 0; dz < 4; ++dz)
                    for (int dx = 0; dx < 4; ++dx) chunkState(o, stand.terrain(x0 + dx, z0 + dz));
            }
            System.err.println("worldgen: golden features: " + kind[0] + ": " + (kind.length - 2) + " areas, " + ran + " features placed");
        }
    }

    /**
     * The kinds of feature that are held to vanilla by placing them over and over: a name, the class, how many
     * attempts at most, and biomes that have features of the kind.
     */
    static final String[][] KINDS_TO_ATTEMPT = {
        {"disk", "DiskFeature", "3000", "minecraft:river", "minecraft:mangrove_swamp", "minecraft:ice_spikes"},
        {"underwater_magma", "UnderwaterMagmaFeature", "3000", "minecraft:ocean", "minecraft:dripstone_caves"},
        {"block_blob", "BlockBlobFeature", "3000", "minecraft:old_growth_pine_taiga", "minecraft:old_growth_spruce_taiga"},
        {"spring_feature", "SpringFeature", "3000", "minecraft:windswept_hills", "minecraft:grove", "minecraft:dripstone_caves"},
        {"lake", "LakeFeature", "3000", "minecraft:plains", "minecraft:desert"},
        {"blue_ice", "BlueIceFeature", "3000", "minecraft:deep_frozen_ocean", "minecraft:frozen_ocean"},
        {"spike", "SpikeFeature", "3000", "minecraft:ice_spikes"},
        {"iceberg", "IcebergFeature", "3000", "minecraft:frozen_ocean", "minecraft:deep_frozen_ocean"},
        {"freeze_top_layer", "SnowAndFreezeFeature", "2", "minecraft:snowy_plains", "minecraft:frozen_ocean", "minecraft:grove", "minecraft:windswept_hills",
            "minecraft:frozen_peaks"},
        {"block_column", "BlockColumnFeature", "300", "minecraft:desert", "minecraft:swamp", "minecraft:ocean", "minecraft:lush_caves", "minecraft:river",
            "minecraft:badlands"},
        {"bamboo", "BambooFeature", "300", "minecraft:bamboo_jungle", "minecraft:jungle"},
        {"vines", "VinesFeature", "300", "minecraft:jungle", "minecraft:lush_caves", "minecraft:swamp"},
        // (By the names of placed features: what they are made of is the kind - corals, the big mushrooms.)
        {"coral", "=minecraft:warm_ocean_vegetation", "300", "minecraft:warm_ocean"},
        {"huge_mushroom", "=minecraft:mushroom_island_vegetation", "300", "minecraft:mushroom_fields"},
        // Trees, by the placed features of the biomes that have them: every trunk and foliage placer, the roots, the decorators.
        {"tree", "=minecraft:trees_plains,minecraft:trees_birch_and_oak_leaf_litter,minecraft:trees_birch,minecraft:birch_tall,minecraft:trees_flower_forest,"
            + "minecraft:trees_taiga,minecraft:trees_snowy,minecraft:trees_grove,minecraft:trees_windswept_hills,minecraft:trees_windswept_forest,"
            + "minecraft:trees_old_growth_pine_taiga,minecraft:trees_old_growth_spruce_taiga,minecraft:trees_savanna,minecraft:trees_windswept_savanna,"
            + "minecraft:trees_jungle,minecraft:trees_sparse_jungle,minecraft:bamboo_vegetation,minecraft:trees_swamp,minecraft:trees_mangrove,"
            + "minecraft:dark_forest_vegetation,minecraft:pale_garden_vegetation,minecraft:trees_cherry,minecraft:trees_meadow,minecraft:trees_badlands,"
            + "minecraft:trees_dappled_forest", "60",
            "minecraft:plains", "minecraft:forest", "minecraft:birch_forest", "minecraft:old_growth_birch_forest", "minecraft:flower_forest",
            "minecraft:taiga", "minecraft:snowy_taiga", "minecraft:grove", "minecraft:windswept_hills", "minecraft:windswept_forest",
            "minecraft:old_growth_pine_taiga", "minecraft:old_growth_spruce_taiga", "minecraft:savanna", "minecraft:windswept_savanna",
            "minecraft:jungle", "minecraft:sparse_jungle", "minecraft:bamboo_jungle", "minecraft:swamp", "minecraft:mangrove_swamp",
            "minecraft:dark_forest", "minecraft:pale_garden", "minecraft:cherry_grove", "minecraft:meadow", "minecraft:wooded_badlands",
            "minecraft:dappled_forest"},
        // Caves: patches of moss and clay, the azalea's roots, dripstone small and large, geodes, lichen, sculk.
        {"vegetation_patch", "=minecraft:lush_caves_ceiling_vegetation,minecraft:lush_caves_clay,minecraft:lush_caves_vegetation,minecraft:pale_moss_patch",
            "120", "minecraft:lush_caves", "minecraft:pale_garden"},
        {"root_system", "=minecraft:rooted_azalea_tree", "400", "minecraft:lush_caves"},
        {"speleothem", "=minecraft:pointed_dripstone,minecraft:sulfur_spike", "200", "minecraft:dripstone_caves", "minecraft:sulfur_caves"},
        {"speleothem_cluster", "=minecraft:dripstone_cluster,minecraft:sulfur_spike_cluster", "120", "minecraft:dripstone_caves", "minecraft:sulfur_caves"},
        {"large_dripstone", "=minecraft:large_dripstone", "200", "minecraft:dripstone_caves"},
        {"geode", "=minecraft:amethyst_geode", "3000", "minecraft:plains", "minecraft:dripstone_caves", "minecraft:ocean"},
        {"multiface_growth", "=minecraft:glow_lichen,minecraft:sculk_vein", "200", "minecraft:dripstone_caves", "minecraft:deep_dark", "minecraft:plains"},
        {"sculk_patch", "=minecraft:sculk_patch_deep_dark", "200", "minecraft:deep_dark"},
        {"monster_room", "=minecraft:monster_room,minecraft:monster_room_deep", "4000", "minecraft:plains", "minecraft:dripstone_caves", "minecraft:forest",
            "minecraft:jungle", "minecraft:windswept_hills"},
        {"simple_block", "SimpleBlockFeature", "300", "minecraft:plains", "minecraft:flower_forest", "minecraft:meadow", "minecraft:forest", "minecraft:swamp",
            "minecraft:taiga", "minecraft:desert", "minecraft:jungle", "minecraft:lush_caves", "minecraft:warm_ocean", "minecraft:mushroom_fields",
            "minecraft:savanna", "minecraft:cherry_grove", "minecraft:old_growth_pine_taiga", "minecraft:sunflower_plains", "minecraft:mangrove_swamp",
            "minecraft:dark_forest", "minecraft:ocean"},
    };

    /**
     * A kind of feature placed again and again in one place: nine chunks of terrain, and every placed feature of
     * the kind that the biomes there have, each tried with source after source - seeded as a decoration seeds a
     * feature, with the number of the attempt added - until a dozen attempts have written something (or the kind's
     * limit of attempts is reached). What rarely happens in a chunk happens here; what has been built is in the way
     * of what comes after. Of every attempt that wrote: what, as one number, and the long its source gave
     * afterwards; then the nine chunks.
     */
    static void attempts(WorldgenGolden.Out o, WorldgenRef.World world, List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps,
                         Map<String, List<int[]>> every) throws Exception {
        o.i32(KINDS_TO_ATTEMPT.length);
        var names = world.registries().lookupOrThrow(net.minecraft.core.registries.Registries.PLACED_FEATURE);
        for (String[] kind : KINDS_TO_ATTEMPT) {
            java.util.function.Predicate<net.minecraft.world.level.levelgen.placement.PlacedFeature> type;
            if (kind[1].startsWith("=")) {
                java.util.Set<String> wanted = java.util.Set.of(kind[1].substring(1).split(","));
                type = placed -> names.getKey(placed) != null && wanted.contains(names.getKey(placed).toString());
            } else {
                Class<?> of = Class.forName("net.minecraft.world.level.levelgen.feature." + kind[1]);
                type = placed -> of.isInstance(placed.feature().value());
            }
            int limit = Integer.parseInt(kind[2]);
            o.str(kind[0]);
            // Where the kind does something: of each biome the first chunk - of the biome's own and a few
            // around them - in which an attempt writes. (Looked for with the attempts themselves, in a stand
            // that is thrown away: what is written down is made again, from nothing.)
            List<int[]> places = new ArrayList<>();
            for (int site = 3; site < kind.length; ++site) {
                List<int[]> candidates = new ArrayList<>();
                for (int ring = 0; ring < 4; ++ring)
                    for (int[] at : every.get(kind[site]))
                        for (int[] step : new int[][] {{0, 0}, {3, 0}, {0, 3}, {-3, 0}, {0, -3}})
                            if (ring > 0 || (step[0] == 0 && step[1] == 0)) candidates.add(new int[] {at[0] + step[0] * ring, at[1] + step[1] * ring});
                for (int[] candidate : candidates) {
                    if (attemptsAt(null, new Stand(world), type, limit, kind[0], candidate[0], candidate[1], steps)[0] == 0) continue;
                    places.add(candidate);
                    break;
                }
            }
            o.i32(places.size());
            int wrote = 0, tried = 0;
            for (int[] place : places) {
                int[] counted = attemptsAt(o, new Stand(world), type, limit, kind[0], place[0], place[1], steps);
                wrote += counted[0];
                tried += counted[1];
            }
            System.err.println("worldgen: golden features: " + kind[0] + ": " + places.size() + " places, " + tried + " attempts, " + wrote + " of them wrote");
        }
    }

    /** The attempts of one kind around one chunk, written to `o` if there is one: {how many wrote, how many there were}. */
    static int[] attemptsAt(WorldgenGolden.Out o, Stand stand, java.util.function.Predicate<net.minecraft.world.level.levelgen.placement.PlacedFeature> type,
                            int limit, String kind, int cx, int cz,
                            List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps) throws Exception {
        int wrote = 0, tried = 0;
        {
            {
                Region region = stand.regionAround(cx, cz);
                ProtoChunk center = stand.chunks.get(Stand.key(cx, cz));
                stand.region = region;
                BlockPos origin = net.minecraft.core.SectionPos.of(center.getPos(), region.getMinSectionY()).origin();
                var random = source(0L);
                long decorationSeed = random.setDecorationSeed(region.getSeed(), origin.getX(), origin.getZ());
                java.util.Set<net.minecraft.core.Holder<net.minecraft.world.level.biome.Biome>> possible = new java.util.HashSet<>();
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dz = -1; dz <= 1; ++dz)
                        for (var section : region.getChunk(cx + dx, cz + dz).getSections()) section.getBiomes().getAll(possible::add);
                possible.retainAll(stand.maker.biomeSource.possibleBiomes());
                var placer = new net.minecraft.world.level.levelgen.placement.FeaturePlacer(region, stand.maker.generator);
                List<int[]> which = new ArrayList<>();
                for (int s = 0; s < steps.size(); ++s) {
                    java.util.TreeSet<Integer> indices = new java.util.TreeSet<>();
                    for (var biome : possible) {
                        var features = stand.maker.generator.getBiomeGenerationSettings(biome).features();
                        if (s >= features.size()) continue;
                        for (var feature : features.get(s)) indices.add(steps.get(s).indexMapping().applyAsInt(feature.value()));
                    }
                    for (int index : indices)
                        if (type.test(steps.get(s).features().get(index))) which.add(new int[] {s, index});
                }
                if (o != null) {
                    o.i32(cx);
                    o.i32(cz);
                    o.i32(which.size());
                }
                for (int[] at : which) {
                    var placed = steps.get(at[0]).features().get(at[1]);
                    List<long[]> writers = new ArrayList<>();
                    int attempt = 0;
                    for (; attempt < limit && (attempt < 24 || writers.size() < 12); ++attempt) {
                        random.setFeatureSeed(decorationSeed + attempt, at[1], at[0]);
                        stand.writes = 0xcbf29ce484222325L;
                        stand.logging = true;
                        // WORLDGEN_TRACE=<kind>:<chunk x>:<chunk z>:<attempt>: every block that attempt sets, on stderr.
                        stand.tracing = o != null && (kind + ":" + cx + ":" + cz + ":" + attempt).equals(System.getenv("WORLDGEN_TRACE"));
                        placer.placeWithBiomeCheck(placed, random, origin);
                        stand.logging = stand.tracing = false;
                        if (stand.writes != 0xcbf29ce484222325L) writers.add(new long[] {attempt, stand.writes, random.nextLong()});
                    }
                    if (o != null) {
                        o.u8(at[0]);
                        o.i32(at[1]);
                        o.i32(attempt);
                        o.i32(writers.size());
                        for (long[] w : writers) {
                            o.i32((int) w[0]);
                            o.i64(w[1]);
                            o.i64(w[2]);
                        }
                    }
                    wrote += writers.size();
                    tried += attempt;
                }
                stand.region = null;
                if (o != null)
                    for (int dz = -1; dz <= 1; ++dz)
                        for (int dx = -1; dx <= 1; ++dx) chunkState(o, stand.terrain(cx + dx, cz + dz));
            }
        }
        return new int[] {wrote, tried};
    }

    // ------------------------------------------------------------------------------- tables

    /**
     * What vanilla made of the biomes' feature lists - the part of a decoration that is not in the
     * jar's JSON as it stands:
     *
     *   steps         FeatureSorter.buildFeaturesPerStep: per decoration step, the placed features of all
     *                 biomes in one order. A feature's place in its step is its index, and the index is
     *                 what its random source is seeded with (WorldgenRandom.setFeatureSeed).
     *   biomes        per biome of the overworld, per step: the indices of its features (what
     *                 applyBiomeDecoration collects from the biomes around a chunk), and the indices
     *                 hasFeature answers true for (BiomeFilter: a set of *equal* features, so not
     *                 always the same list)
     *   post_process  BlockBehaviour.Properties.postProcess: the blocks that mark themselves, or the
     *                 block above, to be looked at when their chunk first ticks
     */
    @SuppressWarnings("unchecked")
    static JsonObject features(RegistryAccess registries) throws Exception {
        WorldgenGolden.ChunkMaker maker = new WorldgenGolden.ChunkMaker(WorldgenRef.world(registries, WorldgenRef.SEED_A));
        Field field = ChunkGenerator.class.getDeclaredField("featuresPerStep");
        field.setAccessible(true);
        List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData> steps =
            ((Supplier<List<net.minecraft.world.level.biome.FeatureSorter.StepFeatureData>>) field.get(maker.generator)).get();
        var placed = registries.lookupOrThrow(Registries.PLACED_FEATURE);
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        JsonArray stepsOut = new JsonArray();
        for (net.minecraft.world.level.biome.FeatureSorter.StepFeatureData step : steps) {
            JsonArray names = new JsonArray();
            for (net.minecraft.world.level.levelgen.placement.PlacedFeature feature : step.features()) {
                var key = placed.getResourceKey(feature);
                if (key.isEmpty()) WorldgenRef.problem("a biome has a placed feature that is in no registry: " + feature);
                names.add(key.map(k -> k.identifier().toString()).orElse("?"));
            }
            stepsOut.add(names);
        }
        root.add("steps", stepsOut);
        JsonObject biomes = new JsonObject();
        for (var biome : maker.biomeSource.possibleBiomes()) {
            var settings = maker.generator.getBiomeGenerationSettings(biome);
            JsonArray own = new JsonArray(), has = new JsonArray();
            for (int s = 0; s < steps.size(); ++s) {
                JsonArray indices = new JsonArray();
                if (s < settings.features().size())
                    for (var feature : settings.features().get(s)) indices.add(steps.get(s).indexMapping().applyAsInt(feature.value()));
                own.add(indices);
                JsonArray answered = new JsonArray();
                for (int i = 0; i < steps.get(s).features().size(); ++i)
                    if (settings.hasFeature(steps.get(s).features().get(i))) answered.add(i);
                has.add(answered);
            }
            JsonObject of = new JsonObject();
            of.add("features", own);
            of.add("has", has);
            biomes.add(biome.getRegisteredName(), of);
        }
        root.add("biomes", biomes);
        JsonArray self = new JsonArray(), above = new JsonArray();
        for (net.minecraft.world.level.block.Block block : net.minecraft.core.registries.BuiltInRegistries.BLOCK) {
            int kind = -1;
            for (BlockState state : block.getStateDefinition().getPossibleStates()) {
                BlockPos at = state.getPostProcessPos(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO);
                int k = at == null ? 0 : at.equals(BlockPos.ZERO) ? 1 : at.equals(BlockPos.ZERO.above()) ? 2 : 3;
                if (k == 3 || (kind >= 0 && kind != k)) WorldgenRef.problem(block + " marks a block for its chunk's first tick in a way the tables have no word for");
                kind = k;
            }
            String name = net.minecraft.core.registries.BuiltInRegistries.BLOCK.getKey(block).toString();
            if (kind == 1) self.add(name);
            if (kind == 2) above.add(name);
        }
        JsonObject post = new JsonObject();
        post.add("self", self);
        post.add("above", above);
        root.add("post_process", post);
        // BlockState.getFaceOcclusionShape(direction) is a whole face: a bit a direction (its ordinal), for every state.
        // (Not the light engine's question, which mcgen.py blockdata answers: that one is asked only of states
        // that are lit by their shape.)
        JsonArray occludes = new JsonArray();
        for (int id = 0; id < net.minecraft.world.level.block.Block.BLOCK_STATE_REGISTRY.size(); ++id) occludes.add(faceOccludes(net.minecraft.world.level.block.Block.stateById(id)));
        root.add("face_occludes", occludes);
        // SnowLayerBlock.canSurvive, asked of the state below: 1 where a layer of snow may lie on it.
        JsonArray snow = new JsonArray();
        for (int id = 0; id < net.minecraft.world.level.block.Block.BLOCK_STATE_REGISTRY.size(); ++id) snow.add(snowSurvivesOn(net.minecraft.world.level.block.Block.stateById(id)) ? 1 : 0);
        root.add("snow_survives_on", snow);
        // VineBlock.isAcceptableNeighbour (MultifaceBlock.canAttachTo): what grows on faces holds on to this state,
        // which lies in a direction from it - a bit a direction, as above.
        JsonArray attaches = new JsonArray();
        for (int id = 0; id < net.minecraft.world.level.block.Block.BLOCK_STATE_REGISTRY.size(); ++id) attaches.add(vineAttaches(net.minecraft.world.level.block.Block.stateById(id)));
        root.add("vine_attaches", attaches);
        // The blocks of every block tag in the order the game holds them: what a provider draws one of
        // (RandomBlockProvider: HolderSet.getRandomElement) is the n-th of them.
        JsonObject tags = new JsonObject();
        net.minecraft.core.registries.BuiltInRegistries.BLOCK.getTags().forEach(named -> {
            JsonArray blocks = new JsonArray();
            named.forEach(holder -> blocks.add(holder.getRegisteredName()));
            tags.add(named.key().location().toString(), blocks);
        });
        root.add("block_tags", tags);
        root.add("noise_providers", noiseProviders(registries));
        // GeodeFeature.NOISE_PARAMETERS as a noise is made of it: how often and how much each layer counts (which is
        // the same for every seed; the seed is the layers' gradients, which src/worldgen makes).
        java.lang.reflect.Field geode = field(net.minecraft.world.level.levelgen.feature.GeodeFeature.class, "NOISE_PARAMETERS");
        Object stack = ((net.minecraft.world.level.levelgen.synth.NormalNoise) geode.get(null)).create(
            new net.minecraft.world.level.levelgen.WorldgenRandom(new net.minecraft.world.level.levelgen.LegacyRandomSource(0L)));
        JsonObject geodeNoise = new JsonObject();
        geodeNoise.addProperty("first_octave", -4);
        JsonArray geodeLayers = new JsonArray();
        for (com.google.gson.JsonElement layer : layers(stack)) {
            JsonObject of = new JsonObject();
            of.add("frequency", layer.getAsJsonObject().get("frequency"));
            of.add("amplitude", layer.getAsJsonObject().get("amplitude"));
            geodeLayers.add(of);
        }
        geodeNoise.add("layers", geodeLayers);
        root.add("geode_noise", geodeNoise);
        // BlockState.isCollisionShapeFullBlock, a state at a time (what sculk may begin beside).
        JsonArray collisionFull = new JsonArray();
        for (int id = 0; id < net.minecraft.world.level.block.Block.BLOCK_STATE_REGISTRY.size(); ++id)
            collisionFull.add(net.minecraft.world.level.block.Block.stateById(id).isCollisionShapeFullBlock(
                net.minecraft.world.level.EmptyBlockGetter.INSTANCE, BlockPos.ZERO) ? 1 : 0);
        root.add("collision_full", collisionFull);
        return root;
    }

    /** A noise as it is once it is made (NoiseStack): its layers - frequency, amplitude, offsets, permutation - numbers as their bits. */
    static JsonArray layers(Object noise) throws Exception {
        JsonArray out = new JsonArray();
        Object[] made = (Object[]) field(net.minecraft.world.level.levelgen.synth.NoiseStack.class, "layers").get(noise);
        for (Object layer : made) {
            Object gradient = field(layer.getClass(), "noise").get(layer);
            if (gradient.getClass() != net.minecraft.world.level.levelgen.synth.PerlinNoise.class)
                throw new IllegalStateException("a layer of " + gradient.getClass() + " in a state provider's noise");
            JsonObject of = new JsonObject();
            of.addProperty("frequency", Long.toString(Double.doubleToRawLongBits((double) field(layer.getClass(), "frequency").get(layer))));
            of.addProperty("amplitude", Float.floatToRawIntBits((float) field(layer.getClass(), "amplitude").get(layer)));
            JsonArray offsets = new JsonArray();
            for (String name : new String[] {"offsetX", "offsetY", "offsetZ"})
                offsets.add(Long.toString(Double.doubleToRawLongBits((double) field(net.minecraft.world.level.levelgen.synth.GradientNoise.class, name).get(gradient))));
            of.add("offsets", offsets);
            JsonArray perms = new JsonArray();
            for (byte b : (byte[]) field(net.minecraft.world.level.levelgen.synth.GradientNoise.class, "perms").get(gradient)) perms.add(b & 0xFF);
            of.add("perms", perms);
            out.add(of);
        }
        return out;
    }

    static java.lang.reflect.Field field(Class<?> owner, String name) throws Exception {
        java.lang.reflect.Field f = owner.getDeclaredField(name);
        f.setAccessible(true);
        return f;
    }

    /**
     * The state providers of the data that go by a noise (NoiseBasedStateProvider): each as it is written, and the
     * noises it made of its seed - the same in every world, so they are tables to src/worldgen and not code.
     */
    static JsonArray noiseProviders(RegistryAccess registries) throws Exception {
        JsonArray out = new JsonArray();
        var ops = registries.createSerializationContext(com.mojang.serialization.JsonOps.INSTANCE);
        java.util.Set<String> seen = new java.util.HashSet<>();
        java.util.ArrayDeque<com.google.gson.JsonElement> open = new java.util.ArrayDeque<>();
        for (String kind : new String[] {"worldgen/placed_feature", "worldgen/feature", "worldgen/block_state_provider"})
            for (var found : resources.listResources(kind, id -> id.getPath().endsWith(".json")).entrySet())
                try (java.io.Reader reader = found.getValue().openAsReader()) {
                    open.add(com.google.gson.JsonParser.parseReader(reader));
                }
        while (!open.isEmpty()) {
            com.google.gson.JsonElement e = open.poll();
            if (e.isJsonArray()) e.getAsJsonArray().forEach(open::add);
            if (!e.isJsonObject()) continue;
            e.getAsJsonObject().asMap().values().forEach(open::add);
            if (!e.getAsJsonObject().has("type") || !e.getAsJsonObject().has("noise") || !e.getAsJsonObject().has("seed")) continue;
            var parsed = net.minecraft.world.level.levelgen.feature.stateproviders.BlockStateProvider.DIRECT_CODEC.parse(ops, e).result();
            if (parsed.isEmpty() || !(parsed.get() instanceof net.minecraft.world.level.levelgen.feature.stateproviders.NoiseBasedStateProvider provider)) continue;
            StringBuilder name = new StringBuilder();
            canon(e, name);
            if (!seen.add(name.toString())) continue;
            JsonObject of = new JsonObject();
            of.addProperty("provider", name.toString());
            of.add("noise", layers(field(net.minecraft.world.level.levelgen.feature.stateproviders.NoiseBasedStateProvider.class, "noise").get(provider)));
            if (provider instanceof net.minecraft.world.level.levelgen.feature.stateproviders.DualNoiseProvider)
                of.add("slow_noise", layers(field(net.minecraft.world.level.levelgen.feature.stateproviders.DualNoiseProvider.class, "slowNoise").get(provider)));
            out.add(of);
        }
        return out;
    }

    /** A level in which one block state is everywhere, and nothing else is known - as the tables of mcgen.py gamedata see a block. */
    static net.minecraft.world.level.LevelAccessor everywhere(BlockState[] asked) {
        return (net.minecraft.world.level.LevelAccessor) java.lang.reflect.Proxy.newProxyInstance(
            WorldgenFeatures.class.getClassLoader(), new Class<?>[] {net.minecraft.world.level.LevelAccessor.class}, (proxy, method, args) -> {
                if (method.getName().equals("getBlockState")) return asked[0];
                if (method.getName().equals("getFluidState")) return asked[0].getFluidState();
                if (method.getDeclaringClass().isInstance(net.minecraft.world.level.EmptyBlockGetter.INSTANCE))
                    return method.invoke(net.minecraft.world.level.EmptyBlockGetter.INSTANCE, args);
                throw new UnsupportedOperationException(method.getName());
            });
    }

    static int vineAttaches(BlockState neighbour) {
        int bits = 0;
        var level = everywhere(new BlockState[] {neighbour});
        for (net.minecraft.core.Direction direction : net.minecraft.core.Direction.values())
            if (net.minecraft.world.level.block.VineBlock.isAcceptableNeighbour(level, BlockPos.ZERO, direction)) bits |= 1 << direction.ordinal();
        return bits;
    }

    static boolean snowSurvivesOn(BlockState below) {
        return net.minecraft.world.level.block.Blocks.SNOW.defaultBlockState().canSurvive(everywhere(new BlockState[] {below}), BlockPos.ZERO);
    }

    static int faceOccludes(BlockState state) {
        int bits = 0;
        for (net.minecraft.core.Direction direction : net.minecraft.core.Direction.values()) {
            var shape = state.getFaceOcclusionShape(direction);
            if (shape != net.minecraft.world.phys.shapes.Shapes.empty() && net.minecraft.world.level.block.Block.isShapeFullBlock(shape)) bits |= 1 << direction.ordinal();
        }
        return bits;
    }

    /** Which sections of WorldgenRef want the game loaded with its resources kept. */
    static final List<String> SECTIONS = List.of("features", "census", "replay", "golden", "survive");
}
