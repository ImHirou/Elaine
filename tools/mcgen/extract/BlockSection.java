// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen gamedata, sections "blocks" and "fluids": what a block (a fluid) is made of.
//
// "hooks" names the class that overrides each BlockBehaviour/Block method, by exact signature: the
// list of behaviour a port of that block has to cover. "settings" is the BlockBehaviour.Properties
// every block is built from; "fields" is what its own class adds - for most classes exactly their
// constructor arguments (the fluid of a LiquidBlock, the base state of a stair, the burn odds of fire).

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.util.ArrayList;
import java.util.Collection;
import java.util.List;
import net.minecraft.core.BlockPos;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.SoundType;
import net.minecraft.world.level.block.entity.BlockEntityType;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.StateHolder;
import net.minecraft.world.level.block.state.properties.BooleanProperty;
import net.minecraft.world.level.block.state.properties.IntegerProperty;
import net.minecraft.world.level.block.state.properties.Property;
import net.minecraft.world.level.material.FlowingFluid;
import net.minecraft.world.level.material.Fluid;
import net.minecraft.world.level.material.FluidState;
import net.minecraft.world.phys.Vec3;

final class BlockSection {
    private BlockSection() {}

    static String key(net.minecraft.resources.Identifier id) {
        return String.valueOf(id);
    }

    static String sound(net.minecraft.sounds.SoundEvent e) {
        return key(BuiltInRegistries.SOUND_EVENT.getKey(e));
    }

    /**
     * Properties in the order that makes the state id: `first_state + sum(index[i] * stride[i])`, the
     * last property changing fastest. Checked against every state before it is written.
     */
    static <O, S extends StateHolder<O, S>> JsonArray properties(String owner, Collection<Property<?>> props, List<S> states,
                                                                 java.util.function.ToIntFunction<S> idOf) {
        List<Property<?>> list = new ArrayList<>(props);
        int[] stride = new int[list.size()];
        int total = 1;
        for (int i = list.size() - 1; i >= 0; --i) {
            stride[i] = total;
            total *= list.get(i).getPossibleValues().size();
        }
        boolean formula = total == states.size();
        int first = idOf.applyAsInt(states.get(0));
        for (int k = 0; formula && k < states.size(); ++k) {
            S s = states.get(k);
            int at = 0;
            for (int i = 0; i < list.size(); ++i) at += indexOf(list.get(i), s) * stride[i];
            formula = at == k && idOf.applyAsInt(s) == first + k;
        }
        if (!formula) Dump.problem(owner + ": state ids do not follow the property order - every table indexed by properties is wrong");

        JsonArray out = new JsonArray();
        for (int i = 0; i < list.size(); ++i) {
            Property<?> p = list.get(i);
            JsonObject o = new JsonObject();
            o.addProperty("name", p.getName());
            o.addProperty("type", p instanceof BooleanProperty ? "bool" : p instanceof IntegerProperty ? "int" : "enum");
            if (!(p instanceof BooleanProperty) && !(p instanceof IntegerProperty)) o.addProperty("enum", Dump.simple(p.getValueClass()));
            o.addProperty("stride", stride[i]);
            o.add("values", valueNames(p));
            out.add(o);
        }
        return out;
    }

    private static <T extends Comparable<T>> int indexOf(Property<T> p, StateHolder<?, ?> s) {
        return p.getPossibleValues().indexOf(s.getValue(p));
    }

    private static <T extends Comparable<T>> JsonArray valueNames(Property<T> p) {
        JsonArray out = new JsonArray();
        for (T v : p.getPossibleValues()) out.add(p.getName(v));
        return out;
    }

    private static final Dump.Hooks HOOKS = new Dump.Hooks(BlockBehaviour.class, Block.class);

    static JsonObject run(GameData.Env env) {
        JsonArray blocks = new JsonArray();
        int next = 0;
        for (Block b : BuiltInRegistries.BLOCK) {
            String name = key(BuiltInRegistries.BLOCK.getKey(b));
            List<BlockState> states = b.getStateDefinition().getPossibleStates();
            BlockState def = b.defaultBlockState();
            JsonObject o = new JsonObject();
            o.addProperty("name", name);
            o.addProperty("id", BuiltInRegistries.BLOCK.getId(b));
            o.add("class", Dump.chain(b.getClass(), BlockBehaviour.class));
            o.add("interfaces", Dump.interfaces(b.getClass()));
            int first = Block.getId(states.get(0));
            if (first != next) Dump.problem(name + ": its states do not follow the previous block's");
            next = first + states.size();
            o.addProperty("first_state", first);
            o.addProperty("state_count", states.size());
            o.addProperty("default_state", Block.getId(def));
            o.add("properties", properties(name, b.getStateDefinition().getProperties(), states, Block::getId));
            o.addProperty("item", key(BuiltInRegistries.ITEM.getKey(b.asItem())));
            o.addProperty("enabled", b.isEnabled(env.features()));
            o.addProperty("description_id", b.getDescriptionId());
            if (b.getLootTable().isPresent()) o.addProperty("loot_table", key(b.getLootTable().get().identifier()));
            for (BlockEntityType<?> t : BuiltInRegistries.BLOCK_ENTITY_TYPE)
                if (t.isValid(def)) o.addProperty("block_entity_type", key(BuiltInRegistries.BLOCK_ENTITY_TYPE.getKey(t)));

            SoundType st = def.getSoundType();
            JsonObject snd = new JsonObject();
            snd.addProperty("volume", st.getVolume());
            snd.addProperty("pitch", st.getPitch());
            snd.addProperty("break", sound(st.getBreakSound()));
            snd.addProperty("step", sound(st.getStepSound()));
            snd.addProperty("place", sound(st.getPlaceSound()));
            snd.addProperty("hit", sound(st.getHitSound()));
            snd.addProperty("fall", sound(st.getFallSound()));
            o.add("sound", snd);

            if (def.hasOffsetFunction()) o.add("offset", offset(b, def));
            o.add("hooks", HOOKS.overridden(b.getClass()));
            // BlockBehaviour.Properties as the block was registered with (what is a lambda there - map
            // colour, light, the offset function - is in states.json as its per-state result).
            JsonObject settings = Dump.fields(Dump.get(b, BlockBehaviour.class, "properties"));
            for (String elsewhere : new String[] {"soundType", "id", "requiredFeatures", "drops", "descriptionId"})
                settings.remove(elsewhere);
            o.add("settings", settings);
            o.add("fields", Dump.fields(b, Block.class));
            blocks.add(o);
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        root.add("hook_names", HOOKS.names());
        root.add("blocks", blocks);
        return root;
    }

    /**
     * BlockBehaviour.Properties.offsetType(): the block is drawn - and for bamboo and dripstone,
     * collides - shifted by a hash of its position. The function is code (BlockBehaviour$OffsetType +
     * Mth.getSeed); these samples are what a port has to reproduce.
     */
    private static JsonObject offset(Block b, BlockState def) {
        JsonObject o = new JsonObject();
        o.addProperty("max_horizontal", (Float) Dump.call(b, "getMaxHorizontalOffset"));
        o.addProperty("max_vertical", (Float) Dump.call(b, "getMaxVerticalOffset"));
        boolean vertical = false;
        JsonArray samples = new JsonArray();
        int[][] at = {{0, 0, 0}, {1, 0, 0}, {0, 0, 1}, {0, 1, 0}, {5, 3, 11}, {-1, 64, -1},
                      {-30000000, -64, 29999999}, {123456, 70, -654321}};
        for (int[] c : at) {
            Vec3 v = def.getOffset(new BlockPos(c[0], c[1], c[2]));
            vertical |= v.y != 0;
            JsonArray row = new JsonArray();
            for (int x : c) row.add(x);
            row.add(v.x);
            row.add(v.y);
            row.add(v.z);
            samples.add(row);
        }
        o.addProperty("type", vertical ? "XYZ" : "XZ");
        o.add("samples", samples);  // [x, y, z, offset x, offset y, offset z]
        return o;
    }

    static JsonObject fluids(GameData.Env env) {
        JsonArray fluids = new JsonArray();
        for (Fluid f : BuiltInRegistries.FLUID) {
            String name = key(BuiltInRegistries.FLUID.getKey(f));
            List<FluidState> states = f.getStateDefinition().getPossibleStates();
            JsonObject o = new JsonObject();
            o.addProperty("name", name);
            o.addProperty("id", BuiltInRegistries.FLUID.getId(f));
            o.add("class", Dump.chain(f.getClass(), Fluid.class));
            o.addProperty("first_state", Fluid.FLUID_STATE_REGISTRY.getId(states.get(0)));
            o.addProperty("state_count", states.size());
            o.addProperty("default_state", Fluid.FLUID_STATE_REGISTRY.getId(f.defaultFluidState()));
            o.add("properties", properties(name, f.getStateDefinition().getProperties(), states, Fluid.FLUID_STATE_REGISTRY::getId));
            o.addProperty("bucket", key(BuiltInRegistries.ITEM.getKey(f.getBucket())));
            if (f instanceof FlowingFluid flowing) {
                o.addProperty("source", key(BuiltInRegistries.FLUID.getKey(flowing.getSource())));
                o.addProperty("flowing", key(BuiltInRegistries.FLUID.getKey(flowing.getFlowing())));
            }
            if (f.getPickupSound().isPresent()) o.addProperty("pickup_sound", sound(f.getPickupSound().get()));
            JsonObject fields = Dump.fields(f, Fluid.class);
            fields.remove("shapes");  // a cache
            o.add("fields", fields);
            fluids.add(o);
        }
        JsonArray states = new JsonArray();
        for (int id = 0; id < Fluid.FLUID_STATE_REGISTRY.size(); ++id) {
            FluidState s = Fluid.FLUID_STATE_REGISTRY.byId(id);
            JsonObject o = new JsonObject();
            o.addProperty("id", id);
            o.addProperty("fluid", key(BuiltInRegistries.FLUID.getKey(s.getType())));
            o.addProperty("empty", s.isEmpty());
            o.addProperty("source", s.isSource());
            o.addProperty("amount", s.getAmount());
            o.addProperty("own_height", s.getOwnHeight());
            if (s.hasProperty(FlowingFluid.FALLING)) o.addProperty("falling", s.getValue(FlowingFluid.FALLING));
            o.addProperty("randomly_ticking", s.isRandomlyTicking());
            o.addProperty("explosion_resistance", s.getExplosionResistance());
            o.addProperty("legacy_block", Block.getId(s.createLegacyBlock()));  // the block state that holds this fluid state
            states.add(o);
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        root.add("fluids", fluids);
        root.add("states", states);
        return root;
    }
}
