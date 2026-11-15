// mcgen blockdata: dump what only exists once vanilla's static registries are built - per block
// state light/fluid/shape properties, per block class hooks, and the item -> block mapping.
//
// It bootstraps the registries exactly like Mojang's data generator (net.minecraft.data.Main):
// SharedConstants.tryDetectVersion() + Bootstrap.bootStrap(). No server, no world, no network.
// Every value below is read through vanilla's own accessors, so nothing here encodes an assumption
// about a block; the only logic is canonicalising shapes into small id tables.
//
// Run through `tools/mcgen/mcgen.py blockdata` (which supplies the class path and Java version).

import com.google.gson.stream.JsonWriter;
import java.io.Writer;
import java.nio.charset.StandardCharsets;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.Collections;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.server.Bootstrap;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.EmptyBlockGetter;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.EntityBlock;
import net.minecraft.world.level.block.GameMasterBlock;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.AABB;
import net.minecraft.world.phys.shapes.Shapes;
import net.minecraft.world.phys.shapes.VoxelShape;

public final class Extract {
    /** Canonical ids for shapes; id 0 is always the empty shape. */
    static final class ShapeTable {
        final Map<String, Integer> ids = new LinkedHashMap<>();
        final List<VoxelShape> shapes = new ArrayList<>();
        final List<List<AABB>> boxes = new ArrayList<>();

        ShapeTable() {
            ids.put("", 0);
            shapes.add(Shapes.empty());
            boxes.add(List.of());
        }

        int id(VoxelShape s) {
            if (s.isEmpty()) return 0;
            // optimize() merges boxes, so shapes built differently but geometrically equal share an id.
            // The representative keeps the ORIGINAL object: Shapes.faceShapeOccludes() tests identity
            // with Shapes.block() before it looks at geometry.
            List<AABB> b = new ArrayList<>(s.optimize().toAabbs());
            List<String> parts = new ArrayList<>();
            for (AABB a : b) parts.add(a.minX + "," + a.minY + "," + a.minZ + "," + a.maxX + "," + a.maxY + "," + a.maxZ);
            Collections.sort(parts);
            String key = String.join(";", parts);
            Integer have = ids.get(key);
            if (have != null) return have;
            int id = shapes.size();
            ids.put(key, id);
            shapes.add(s);
            b.sort((x, y) -> {
                double[] p = {x.minY, x.minZ, x.minX, x.maxY, x.maxZ, x.maxX}, q = {y.minY, y.minZ, y.minX, y.maxY, y.maxZ, y.maxX};
                return Arrays.compare(p, q);
            });
            boxes.add(b);
            return id;
        }
    }

    /** Simple name of the class that declares `name` with these parameters, searching up from `c`. */
    static String declaring(Class<?> c, String name) {
        for (Class<?> k = c; k != null; k = k.getSuperclass())
            for (java.lang.reflect.Method m : k.getDeclaredMethods())
                if (m.getName().equals(name) && !m.isBridge() && !m.isSynthetic()) return simple(k);
        return "?";
    }

    static String simple(Class<?> k) {
        String n = k.getName();
        return n.substring(n.lastIndexOf('.') + 1);
    }

    // Hooks whose overrides decide what a block change must do beyond storing the new state.
    static final String[] HOOKS = {
        "getStateForPlacement", "canSurvive", "setPlacedBy", "canBeReplaced", "playerWillDestroy", "updateShape",
        "onPlace", "affectNeighborsAfterRemoval", "neighborChanged", "useItemOn", "useWithoutItem", "getFluidState",
        "tick", "randomTick", "entityInside",
    };

    public static void main(String[] args) throws Exception {
        Path out = Path.of(args[0]);
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();

        final Direction[] dirs = Direction.values();  // DOWN, UP, NORTH, SOUTH, WEST, EAST = light propagation order
        final int n = Block.BLOCK_STATE_REGISTRY.size();
        ShapeTable faces = new ShapeTable();
        ShapeTable collision = new ShapeTable();
        Map<String, Integer> tupleIds = new LinkedHashMap<>();
        List<int[]> tuples = new ArrayList<>();
        tupleIds.put("0,0,0,0,0,0", 0);
        tuples.add(new int[6]);

        int[] air = new int[n], emission = new int[n], dampening = new int[n], canOcclude = new int[n], useShape = new int[n];
        int[] skyDown = new int[n], solidRender = new int[n], solid = new int[n], replaceable = new int[n], blockEntity = new int[n];
        int[] fluid = new int[n], fluidSource = new int[n], fluidLegacy = new int[n], coll = new int[n], occl = new int[n];
        int[] randomTicks = new int[n], blockOf = new int[n];

        for (int id = 0; id < n; ++id) {
            BlockState s = Block.BLOCK_STATE_REGISTRY.byId(id);
            if (s == null) throw new IllegalStateException("hole in the block state registry at " + id);
            Block b = s.getBlock();
            blockOf[id] = BuiltInRegistries.BLOCK.getId(b);
            air[id] = s.isAir() ? 1 : 0;
            emission[id] = s.getLightEmission();
            dampening[id] = s.getLightDampening();
            canOcclude[id] = s.canOcclude() ? 1 : 0;
            useShape[id] = s.useShapeForLightOcclusion() ? 1 : 0;
            skyDown[id] = s.propagatesSkylightDown() ? 1 : 0;
            solidRender[id] = s.isSolidRender() ? 1 : 0;
            solid[id] = s.isSolid() ? 1 : 0;
            replaceable[id] = s.canBeReplaced() ? 1 : 0;
            blockEntity[id] = s.hasBlockEntity() ? 1 : 0;
            randomTicks[id] = s.isRandomlyTicking() ? 1 : 0;
            FluidState f = s.getFluidState();
            fluid[id] = BuiltInRegistries.FLUID.getId(f.getType());
            fluidSource[id] = f.isSource() ? 1 : 0;
            // What Level.removeBlock() leaves behind: air, or the block of the fluid a waterlogged state holds.
            fluidLegacy[id] = Block.getId(f.createLegacyBlock());
            coll[id] = b.hasDynamicShape() ? -1 : collision.id(s.getCollisionShape(EmptyBlockGetter.INSTANCE, BlockPos.ZERO));

            // LightEngine.getOcclusionShape(): empty unless canOcclude && useShapeForLightOcclusion.
            int[] t = new int[6];
            if (s.canOcclude() && s.useShapeForLightOcclusion())
                for (Direction d : dirs) t[d.ordinal()] = faces.id(s.getFaceOcclusionShape(d));
            String key = t[0] + "," + t[1] + "," + t[2] + "," + t[3] + "," + t[4] + "," + t[5];
            Integer tid = tupleIds.get(key);
            if (tid == null) {
                tid = tuples.size();
                tupleIds.put(key, tid);
                tuples.add(t);
            }
            occl[id] = tid;
        }

        try (Writer w = Files.newBufferedWriter(out, StandardCharsets.UTF_8); JsonWriter j = new JsonWriter(w)) {
            j.beginObject();
            j.name("version").value(SharedConstants.getCurrentVersion().name());
            j.name("state_count").value(n);
            j.name("directions").beginArray();
            for (Direction d : dirs) j.value(d.getName());
            j.endArray();

            // face_occludes[a][b] = Shapes.faceShapeOccludes(face shape a, face shape b), as rows of '0'/'1'.
            j.name("face_shapes").beginArray();
            for (List<AABB> b : faces.boxes) boxes(j, b);
            j.endArray();
            j.name("face_occludes").beginArray();
            for (VoxelShape a : faces.shapes) {
                StringBuilder row = new StringBuilder();
                for (VoxelShape b : faces.shapes) row.append(Shapes.faceShapeOccludes(a, b) ? '1' : '0');
                j.value(row.toString());
            }
            j.endArray();
            j.name("occlusion_tuples").beginArray();
            for (int[] t : tuples) ints(j, t);
            j.endArray();
            j.name("collision_shapes").beginArray();
            for (List<AABB> b : collision.boxes) boxes(j, b);
            j.endArray();

            j.name("states").beginObject();
            col(j, "block", blockOf);
            col(j, "air", air);
            col(j, "emission", emission);
            col(j, "dampening", dampening);
            col(j, "can_occlude", canOcclude);
            col(j, "use_shape_for_light_occlusion", useShape);
            col(j, "propagates_skylight_down", skyDown);
            col(j, "solid_render", solidRender);
            col(j, "solid", solid);
            col(j, "replaceable", replaceable);
            col(j, "has_block_entity", blockEntity);
            col(j, "randomly_ticking", randomTicks);
            col(j, "fluid", fluid);
            col(j, "fluid_source", fluidSource);
            col(j, "fluid_legacy_block", fluidLegacy);
            col(j, "collision_shape", coll);
            col(j, "occlusion_tuple", occl);
            j.endObject();

            j.name("fluids").beginArray();
            for (var f : BuiltInRegistries.FLUID) j.value(BuiltInRegistries.FLUID.getKey(f).toString());
            j.endArray();

            j.name("blocks").beginArray();
            for (Block b : BuiltInRegistries.BLOCK) {
                BlockState def = b.defaultBlockState();
                j.beginObject();
                j.name("name").value(BuiltInRegistries.BLOCK.getKey(b).toString());
                j.name("id").value(BuiltInRegistries.BLOCK.getId(b));
                j.name("class").value(simple(b.getClass()));
                j.name("default_state").value(Block.getId(def));
                j.name("dynamic_shape").value(b.hasDynamicShape());
                j.name("game_master").value(b instanceof GameMasterBlock);
                j.name("entity_block").value(b instanceof EntityBlock);
                j.name("item").value(BuiltInRegistries.ITEM.getKey(b.asItem()).toString());
                j.name("destroy_time").value(b.defaultDestroyTime());
                j.name("explosion_resistance").value(b.getExplosionResistance());
                j.name("friction").value(b.getFriction());
                SoundType st = def.getSoundType();
                j.name("sound").beginObject();
                j.name("volume").value(st.getVolume());
                j.name("pitch").value(st.getPitch());
                j.name("place").value(String.valueOf(BuiltInRegistries.SOUND_EVENT.getKey(st.getPlaceSound())));
                j.name("break").value(String.valueOf(BuiltInRegistries.SOUND_EVENT.getKey(st.getBreakSound())));
                j.endObject();
                j.name("hooks").beginObject();
                for (String h : HOOKS) j.name(h).value(declaring(b.getClass(), h));
                j.endObject();
                if (def.hasProperty(BlockStateProperties.AXIS)) {
                    j.name("axis_states").beginArray();
                    for (Direction.Axis a : Direction.Axis.values()) j.value(Block.getId(def.setValue(BlockStateProperties.AXIS, a)));
                    j.endArray();
                }
                j.endObject();
            }
            j.endArray();

            j.name("items").beginArray();
            for (Item it : BuiltInRegistries.ITEM) {
                j.beginObject();
                j.name("name").value(BuiltInRegistries.ITEM.getKey(it).toString());
                j.name("id").value(BuiltInRegistries.ITEM.getId(it));
                j.name("class").value(simple(it.getClass()));
                if (it instanceof BlockItem bi) j.name("block").value(BuiltInRegistries.BLOCK.getKey(bi.getBlock()).toString());
                j.endObject();
            }
            j.endArray();
            j.endObject();
        }
        System.out.println("blockdata: " + n + " states, " + faces.shapes.size() + " face shapes, " + tuples.size()
            + " occlusion tuples, " + collision.shapes.size() + " collision shapes");
        System.out.flush();
        // Bootstrap starts non-daemon worker pools; nothing else is pending.
        System.exit(0);
    }

    static void ints(JsonWriter j, int[] v) throws java.io.IOException {
        j.beginArray();
        for (int x : v) j.value(x);
        j.endArray();
    }

    static void col(JsonWriter j, String name, int[] v) throws java.io.IOException {
        j.name(name);
        ints(j, v);
    }

    static void boxes(JsonWriter j, List<AABB> b) throws java.io.IOException {
        j.beginArray();
        for (AABB a : b) {
            j.beginArray();
            j.value(a.minX).value(a.minY).value(a.minZ).value(a.maxX).value(a.maxY).value(a.maxZ);
            j.endArray();
        }
        j.endArray();
    }
}
