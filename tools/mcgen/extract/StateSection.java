// mcgen gamedata, section "states": one column per question, indexed by block state id.
//
// Everything is asked of vanilla with an empty world around the block (EmptyBlockGetter, BlockPos.ZERO,
// CollisionContext.empty()), which is what the answer is whenever it does not depend on neighbours, a
// block entity or the entity asking. Blocks where it does are the ones whose class overrides the hook
// in question - see "hooks" in blocks.json.

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import java.util.Optional;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.entity.ai.village.poi.PoiType;
import net.minecraft.world.entity.ai.village.poi.PoiTypes;
import net.minecraft.world.level.BlockGetter;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.Mirror;
import net.minecraft.world.level.block.Rotation;
import net.minecraft.world.level.block.SupportType;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.entity.BlockEntityType;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.NoteBlockInstrument;
import net.minecraft.world.level.levelgen.Heightmap;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.level.material.PushReaction;
import net.minecraft.world.level.pathfinder.PathComputationType;
import net.minecraft.world.level.pathfinder.PathType;
import net.minecraft.world.level.pathfinder.WalkNodeEvaluator;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.Vec3;
import net.minecraft.world.phys.shapes.CollisionContext;
import net.minecraft.world.phys.shapes.VoxelShape;

final class StateSection {
    private StateSection() {}

    /** A world that is air except for one block at the origin. */
    record OneBlock(BlockState state) implements BlockGetter {
        @Override
        public BlockEntity getBlockEntity(BlockPos pos) {
            return null;
        }

        @Override
        public BlockState getBlockState(BlockPos pos) {
            return pos.equals(BlockPos.ZERO) ? state : Blocks.AIR.defaultBlockState();
        }

        @Override
        public FluidState getFluidState(BlockPos pos) {
            return getBlockState(pos).getFluidState();
        }

        @Override
        public int getHeight() {
            return 384;
        }

        @Override
        public int getMinY() {
            return -64;
        }
    }

    /** WalkNodeEvaluator.getPathTypeFromState is protected. */
    static final class PathProbe extends WalkNodeEvaluator {
        static PathType of(BlockState state) {
            return getPathTypeFromState(new OneBlock(state), BlockPos.ZERO);
        }
    }

    static <E extends Enum<E>> JsonArray names(E[] values) {
        JsonArray out = new JsonArray();
        for (E e : values) out.add(e.name());
        return out;
    }

    private static final String[] SHAPE_KINDS = {"outline_shape", "collision_shape", "support_shape", "visual_shape", "interaction_shape"};

    private static VoxelShape shape(BlockState s, int kind, BlockPos p) {
        BlockGetter g = EmptyBlockGetter.INSTANCE;
        return switch (kind) {
            case 0 -> s.getShape(g, p);
            case 1 -> s.getCollisionShape(g, p);
            case 2 -> s.getBlockSupportShape(g, p);
            case 3 -> s.getVisualShape(g, p, CollisionContext.empty());
            default -> s.getInteractionShape(g, p);
        };
    }

    /**
     * Blocks with an offset function (bamboo, pointed dripstone, flowers) shift some of their shapes
     * by a per-position amount. Undo it: the stored shape is the one at offset zero, proven by putting
     * two different offsets back with vanilla's arithmetic (`coordinate + offset`) and getting
     * vanilla's own coordinates bit for bit. Returns null when the shape does not move with the offset.
     */
    private static List<AABB> unshifted(BlockState s, int kind, BlockPos other) {
        List<AABB> at0 = shape(s, kind, BlockPos.ZERO).optimize().toAabbs();
        List<AABB> at1 = shape(s, kind, other).optimize().toAabbs();
        if (at0.equals(at1)) return null;
        Vec3 o0 = s.getOffset(BlockPos.ZERO), o1 = s.getOffset(other);
        List<AABB> base = new ArrayList<>();
        boolean exact = at0.size() == at1.size();
        for (int i = 0; exact && i < at0.size(); ++i) {
            AABB a = at0.get(i), b = at1.get(i);
            double[] lo = {snap(a.minX - o0.x), snap(a.minY - o0.y), snap(a.minZ - o0.z)};
            double[] hi = {snap(a.maxX - o0.x), snap(a.maxY - o0.y), snap(a.maxZ - o0.z)};
            exact = lo[0] + o0.x == a.minX && lo[1] + o0.y == a.minY && lo[2] + o0.z == a.minZ
                && hi[0] + o0.x == a.maxX && hi[1] + o0.y == a.maxY && hi[2] + o0.z == a.maxZ
                && lo[0] + o1.x == b.minX && lo[1] + o1.y == b.minY && lo[2] + o1.z == b.minZ
                && hi[0] + o1.x == b.maxX && hi[1] + o1.y == b.maxY && hi[2] + o1.z == b.maxZ;
            base.add(new AABB(lo[0], lo[1], lo[2], hi[0], hi[1], hi[2]));
        }
        if (!exact) {
            Dump.problem("state " + Block.getId(s) + " (" + s + "): " + SHAPE_KINDS[kind]
                + " moves with the position but not by getOffset()");
            return null;
        }
        return base;
    }

    /** Shapes are authored in 1/16 (sometimes 1/32) of a block: the nearest 1/4096 is the authored value. */
    private static double snap(double v) {
        return Math.rint(v * 4096.0) / 4096.0;
    }

    @SuppressWarnings("deprecation")  // BlockState.liquid(): deprecated in vanilla and still what vanilla asks
    static JsonObject run(GameData.Env env) {
        final Direction[] dirs = Direction.values();
        final SupportType[] supports = SupportType.values();
        final PathComputationType[] pathKinds = PathComputationType.values();
        final Heightmap.Types[] heightmaps = Heightmap.Types.values();
        final Rotation[] rotations = {Rotation.CLOCKWISE_90, Rotation.CLOCKWISE_180, Rotation.COUNTERCLOCKWISE_90};
        final Mirror[] mirrors = {Mirror.LEFT_RIGHT, Mirror.FRONT_BACK};
        final BlockGetter g = EmptyBlockGetter.INSTANCE;
        final BlockPos p = BlockPos.ZERO;
        final int n = Block.BLOCK_STATE_REGISTRY.size();

        Dump.ShapeTable shapes = new Dump.ShapeTable();
        Map<String, int[]> col = new LinkedHashMap<>();
        Map<String, JsonArray> floats = new LinkedHashMap<>();
        String[] ints = {
            "block", "outline_shape", "collision_shape", "support_shape", "visual_shape", "interaction_shape", "offset_shapes",
            "sturdy_full", "sturdy_center", "sturdy_rigid", "collision_full_block", "large_collision_shape", "redstone_conductor",
            "suffocating", "signal_source", "analog_output", "ignited_by_lava", "liquid", "requires_correct_tool",
            "terrain_particles", "respawn_in", "push_reaction", "instrument", "map_color", "path_type", "pathfindable",
            "heightmap_opaque", "fluid_state", "rotate_cw90", "rotate_180", "rotate_ccw90", "mirror_left_right", "mirror_front_back",
            "poi_type", "block_entity_type",
        };
        for (String name : ints) col.put(name, new int[n]);
        JsonArray destroySpeed = new JsonArray();
        floats.put("destroy_speed", destroySpeed);

        // A second position whose offset differs from the origin's, to tell shapes that move from ones that do not.
        BlockPos other = new BlockPos(5, 3, 11);

        for (int id = 0; id < n; ++id) {
            BlockState s = Block.BLOCK_STATE_REGISTRY.byId(id);
            if (s == null) throw new IllegalStateException("hole in the block state registry at " + id);
            Block b = s.getBlock();
            col.get("block")[id] = BuiltInRegistries.BLOCK.getId(b);

            int moved = 0;
            for (int kind = 0; kind < SHAPE_KINDS.length; ++kind) {
                List<AABB> base = s.hasOffsetFunction() && !s.getOffset(p).equals(s.getOffset(other)) ? unshifted(s, kind, other) : null;
                if (base != null) moved |= 1 << kind;
                col.get(SHAPE_KINDS[kind])[id] = base != null ? shapes.id(base) : shapes.id(shape(s, kind, p));
            }
            col.get("offset_shapes")[id] = moved;

            for (int t = 0; t < supports.length; ++t) {
                int mask = 0;
                for (Direction d : dirs) mask |= s.isFaceSturdy(g, p, d, supports[t]) ? 1 << d.ordinal() : 0;
                col.get(new String[] {"sturdy_full", "sturdy_center", "sturdy_rigid"}[t])[id] = mask;
            }
            col.get("collision_full_block")[id] = s.isCollisionShapeFullBlock(g, p) ? 1 : 0;
            col.get("large_collision_shape")[id] = s.hasLargeCollisionShape() ? 1 : 0;
            col.get("redstone_conductor")[id] = s.isRedstoneConductor(g, p) ? 1 : 0;
            col.get("suffocating")[id] = s.isSuffocating(g, p) ? 1 : 0;
            col.get("signal_source")[id] = s.isSignalSource() ? 1 : 0;
            col.get("analog_output")[id] = s.hasAnalogOutputSignal() ? 1 : 0;
            col.get("ignited_by_lava")[id] = s.ignitedByLava() ? 1 : 0;
            col.get("liquid")[id] = s.liquid() ? 1 : 0;
            col.get("requires_correct_tool")[id] = s.requiresCorrectToolForDrops() ? 1 : 0;
            col.get("terrain_particles")[id] = s.shouldSpawnTerrainParticles() ? 1 : 0;
            col.get("respawn_in")[id] = b.isPossibleToRespawnInThis(s) ? 1 : 0;
            col.get("push_reaction")[id] = s.getPistonPushReaction().ordinal();
            col.get("instrument")[id] = s.instrument().ordinal();
            col.get("map_color")[id] = s.getMapColor(g, p).id;
            col.get("path_type")[id] = PathProbe.of(s).ordinal();
            int mask = 0;
            for (PathComputationType t : pathKinds) mask |= s.isPathfindable(t) ? 1 << t.ordinal() : 0;
            col.get("pathfindable")[id] = mask;
            mask = 0;
            for (Heightmap.Types t : heightmaps) mask |= t.isOpaque().test(s) ? 1 << t.ordinal() : 0;
            col.get("heightmap_opaque")[id] = mask;
            col.get("fluid_state")[id] = Fluid.FLUID_STATE_REGISTRY.getId(s.getFluidState());
            for (int r = 0; r < rotations.length; ++r)
                col.get(new String[] {"rotate_cw90", "rotate_180", "rotate_ccw90"}[r])[id] = Block.getId(s.rotate(rotations[r]));
            col.get("mirror_left_right")[id] = Block.getId(s.mirror(mirrors[0]));
            col.get("mirror_front_back")[id] = Block.getId(s.mirror(mirrors[1]));
            Optional<Holder<PoiType>> poi = PoiTypes.forState(s);
            col.get("poi_type")[id] = poi.isPresent() ? BuiltInRegistries.POINT_OF_INTEREST_TYPE.getId(poi.get().value()) : -1;
            int type = -1;
            if (s.hasBlockEntity())
                for (BlockEntityType<?> t : BuiltInRegistries.BLOCK_ENTITY_TYPE)
                    if (t.isValid(s)) {
                        if (type >= 0) Dump.problem("state " + id + ": two block entity types accept it");
                        type = BuiltInRegistries.BLOCK_ENTITY_TYPE.getId(t);
                    }
            if (s.hasBlockEntity() && type < 0) Dump.problem("state " + id + " (" + s + "): has a block entity of no registered type");
            col.get("block_entity_type")[id] = type;
            destroySpeed.add(s.getDestroySpeed(g, p));
        }

        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        root.addProperty("state_count", n);
        // What the numbers in the columns index.
        JsonObject legend = new JsonObject();
        legend.add("directions", names(dirs));  // bit i of the sturdy_* masks
        legend.add("push_reaction", names(PushReaction.values()));
        legend.add("instrument", names(NoteBlockInstrument.values()));
        legend.add("path_type", names(PathType.values()));
        legend.add("pathfindable", names(pathKinds));  // bit i
        legend.add("heightmap_opaque", names(heightmaps));  // bit i
        JsonArray kinds = new JsonArray();
        for (String k : SHAPE_KINDS) kinds.add(k);
        legend.add("offset_shapes", kinds);  // bit i: that shape is stored at offset zero and moves by BlockState.getOffset(pos)
        root.add("legend", legend);
        root.add("shapes", shapes.json());
        JsonObject columns = new JsonObject();
        for (Map.Entry<String, int[]> e : col.entrySet()) columns.add(e.getKey(), Dump.ints(e.getValue()));
        for (Map.Entry<String, JsonArray> e : floats.entrySet()) columns.add(e.getKey(), e.getValue());
        root.add("columns", columns);
        return root;
    }
}
