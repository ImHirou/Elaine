// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen gamedata, sections "enums", "statics" and "menus": what is found by walking the server jar
// rather than a registry.

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Enumeration;
import java.util.LinkedHashSet;
import java.util.List;
import java.util.Set;
import java.util.TreeMap;
import java.util.function.Supplier;
import java.util.zip.ZipEntry;
import java.util.zip.ZipFile;
import net.minecraft.SharedConstants;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.util.StringRepresentable;
import net.minecraft.world.effect.MobEffect;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityEquipment;
import net.minecraft.world.entity.player.Inventory;
import net.minecraft.world.inventory.AbstractContainerMenu;
import net.minecraft.world.inventory.MenuType;
import net.minecraft.world.inventory.Slot;
import net.minecraft.world.item.Item;
import net.minecraft.world.level.block.entity.BlockEntity;
import net.minecraft.world.level.block.state.BlockBehaviour;
import net.minecraft.world.level.material.Fluid;

final class MiscSection {
    private MiscSection() {}

    // Not part of a running server: data generators, the test framework, world upgrade code.
    private static final String[] SKIP_PACKAGES = {"net.minecraft.data.", "net.minecraft.gametest.", "net.minecraft.util.datafix."};

    private static List<Class<?>> jarClasses;

    /** Every vanilla class of the server jar, loaded but not initialized. */
    static List<Class<?>> jarClasses() throws Exception {
        if (jarClasses != null) return jarClasses;
        Path jar = Path.of(SharedConstants.class.getProtectionDomain().getCodeSource().getLocation().toURI());
        List<String> names = new ArrayList<>();
        try (ZipFile zip = new ZipFile(jar.toFile())) {
            for (Enumeration<? extends ZipEntry> e = zip.entries(); e.hasMoreElements();) {
                String n = e.nextElement().getName();
                if (n.startsWith("net/minecraft/") && n.endsWith(".class")) names.add(n.substring(0, n.length() - 6).replace('/', '.'));
            }
        }
        names.sort(null);
        List<Class<?>> out = new ArrayList<>();
        next:
        for (String n : names) {
            for (String skip : SKIP_PACKAGES)
                if (n.startsWith(skip)) continue next;
            try {
                out.add(Class.forName(n, false, MiscSection.class.getClassLoader()));
            } catch (ClassNotFoundException | LinkageError e) {
                Dump.problem("cannot load " + n + ": " + e);
            }
        }
        return jarClasses = out;
    }

    // ------------------------------------------------------------------------------------ enums

    /** {class: [constant, ...]} in ordinal order; a constant is its name, serialized name and fields. */
    static JsonObject enums(GameData.Env env) throws Exception {
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        JsonObject enums = new JsonObject();
        for (Class<?> c : jarClasses()) {
            if (!c.isEnum()) continue;
            Object[] constants;
            try {
                constants = c.getEnumConstants();
            } catch (LinkageError | RuntimeException e) {
                Dump.problem("enum " + c.getName() + " does not initialize: " + e);
                continue;
            }
            JsonArray list = new JsonArray();
            for (Object k : constants) {
                JsonObject o = new JsonObject();
                o.addProperty("name", ((Enum<?>) k).name());
                if (k instanceof StringRepresentable s) o.addProperty("serialized", s.getSerializedName());
                for (var e : Dump.fields(k).entrySet())
                    if (!o.has(e.getKey())) o.add(e.getKey(), e.getValue());
                list.add(o);
            }
            enums.add(c.getName(), list);
        }
        root.add("enums", enums);
        return root;
    }

    // ------------------------------------------------------------------------------------ statics

    // Classes whose subclasses hold game mechanics in static fields (shape tables, waxing and
    // weathering pairs, dispenser behaviours, pose dimensions, tuning constants).
    private static final Class<?>[] STATIC_BASES = {
        BlockBehaviour.class, Item.class, Entity.class, BlockEntity.class, AbstractContainerMenu.class, Fluid.class, MobEffect.class,
    };

    // The same, for classes that are not under one of those. A name that is gone is a problem: look
    // where its contents moved (`mcgen.py find`) rather than dropping the line.
    private static final String[] STATIC_EXTRA = {
        "net.minecraft.SharedConstants",
        "net.minecraft.world.level.block.LevelEvent",
        "net.minecraft.world.entity.EntityEvent",
        "net.minecraft.core.cauldron.CauldronInteractions",
        "net.minecraft.server.network.ServerGamePacketListenerImpl",
        "net.minecraft.server.network.ServerCommonPacketListenerImpl",
        "net.minecraft.server.level.ServerPlayerGameMode",
        "net.minecraft.server.level.ServerEntity",
        "net.minecraft.server.level.ChunkMap",
        "net.minecraft.server.level.ChunkLevel",
        "net.minecraft.server.level.ServerLevel",
        "net.minecraft.server.players.PlayerList",
        "net.minecraft.server.MinecraftServer",
        "net.minecraft.world.entity.player.Inventory",
        "net.minecraft.world.food.FoodConstants",
        "net.minecraft.world.food.FoodData",
        "net.minecraft.world.damagesource.CombatRules",
        "net.minecraft.world.level.Level",
        "net.minecraft.world.level.NaturalSpawner",
        "net.minecraft.world.level.redstone.Redstone",
        "net.minecraft.world.level.lighting.LightEngine",
        "net.minecraft.world.level.dimension.DimensionType",
        "net.minecraft.world.level.border.WorldBorder",
        "net.minecraft.world.level.block.piston.PistonStructureResolver",
        "net.minecraft.world.level.chunk.LevelChunkSection",
        "net.minecraft.world.level.material.FluidState",
        "net.minecraft.world.entity.ai.attributes.Attributes",
        "net.minecraft.world.level.material.MapColor",
        "net.minecraft.world.level.block.state.properties.BlockSetType",
        "net.minecraft.world.level.block.state.properties.WoodType",
        "net.minecraft.world.level.block.grower.TreeGrower",
        "net.minecraft.world.item.ToolMaterial",
        "net.minecraft.world.item.equipment.ArmorMaterials",
        "net.minecraft.world.entity.EntityType",
        "net.minecraft.world.inventory.Slot",
    };

    static JsonObject statics(GameData.Env env) throws Exception {
        Set<Class<?>> classes = new LinkedHashSet<>();
        for (Class<?> c : jarClasses())
            for (Class<?> base : STATIC_BASES)
                if (base.isAssignableFrom(c)) addWithInterfaces(c, classes);
        for (String name : STATIC_EXTRA)
            try {
                classes.add(Class.forName(name, false, MiscSection.class.getClassLoader()));
            } catch (ClassNotFoundException e) {
                Dump.problem("statics: class " + name + " is gone");
            }

        TreeMap<String, JsonObject> sorted = new TreeMap<>();
        for (Class<?> c : classes) {
            JsonObject o = new JsonObject();
            for (Field f : c.getDeclaredFields()) {
                if (!Modifier.isStatic(f.getModifiers()) || f.isSynthetic() || f.isEnumConstant()) continue;
                if (f.getType() == EntityDataAccessor.class) continue;  // entities.json
                try {
                    f.setAccessible(true);
                    Object v = f.get(null);
                    // A memoized table (WeatheringCopper.NEXT_BY_BLOCK): the table is what is meant.
                    if (v instanceof Supplier<?> s && v.getClass().getName().contains("Memoiz")) v = s.get();
                    if (v instanceof com.google.common.base.Supplier<?> s && v.getClass().getName().contains("Memoiz")) v = s.get();
                    if (v == null) continue;
                    JsonElement e = Dump.enc(v);
                    if (Dump.isOpaque(e) || e.isJsonNull()) continue;
                    o.add(f.getName(), e);
                } catch (ReflectiveOperationException | RuntimeException | LinkageError e) {
                    Dump.problem("statics: " + c.getName() + "." + f.getName() + ": " + e);
                }
            }
            if (o.size() > 0) sorted.put(c.getName(), o);
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        JsonObject out = new JsonObject();
        for (var e : sorted.entrySet()) out.add(e.getKey(), e.getValue());
        root.add("classes", out);
        return root;
    }

    private static void addWithInterfaces(Class<?> c, Set<Class<?>> into) {
        if (c == null || !Dump.vanilla(c) || !into.add(c)) return;
        for (Class<?> i : c.getInterfaces()) addWithInterfaces(i, into);
    }

    // ------------------------------------------------------------------------------------ menus

    private static final Dump.Hooks MENU_HOOKS = new Dump.Hooks(AbstractContainerMenu.class);
    private static final Dump.Hooks SLOT_HOOKS = new Dump.Hooks(Slot.class);

    /**
     * Slots of each menu type, from building the menu the way the client does (MenuType.create with a
     * bare inventory). There is no player here: a menu whose constructor asks the player's level for
     * something cannot be built, and says so in "unavailable" - read that one from its class
     * (`mcgen.py pseudo net.minecraft.world.inventory.<Menu> --method '<init>'`).
     */
    static JsonObject menus(GameData.Env env) {
        JsonArray menus = new JsonArray();
        TreeMap<String, JsonObject> slotClasses = new TreeMap<>();
        for (MenuType<?> type : BuiltInRegistries.MENU) {
            JsonObject o = new JsonObject();
            o.addProperty("name", BlockSection.key(BuiltInRegistries.MENU.getKey(type)));
            o.addProperty("id", BuiltInRegistries.MENU.getId(type));
            o.addProperty("enabled", type.isEnabled(env.features()));
            try {
                Inventory inventory = new Inventory(null, new EntityEquipment());
                AbstractContainerMenu menu = type.create(1, inventory);
                o.add("class", Dump.chain(menu.getClass(), Object.class));
                o.add("hooks", MENU_HOOKS.overridden(menu.getClass()));
                Object data = Dump.get(menu, AbstractContainerMenu.class, "dataSlots");
                o.addProperty("data_slots", data instanceof List<?> l ? l.size() : -1);
                JsonArray slots = new JsonArray();
                int playerFirst = -1;
                for (Slot s : menu.slots) {
                    JsonObject j = new JsonObject();
                    j.addProperty("index", s.index);
                    j.addProperty("class", Dump.simple(s.getClass()));
                    if (s.getClass() != Slot.class && !slotClasses.containsKey(Dump.simple(s.getClass()))) {
                        JsonObject k = new JsonObject();
                        k.add("class", Dump.chain(s.getClass(), Object.class));
                        k.add("hooks", SLOT_HOOKS.overridden(s.getClass()));  // mayPlace, getMaxStackSize, onTake...: the slot's rules
                        slotClasses.put(Dump.simple(s.getClass()), k);
                    }
                    boolean player = s.container == inventory;
                    if (player && playerFirst < 0) playerFirst = s.index;
                    j.addProperty("container", player ? "player" : Dump.simple(s.container.getClass()));
                    j.addProperty("slot", s.getContainerSlot());
                    j.addProperty("max_stack", s.getMaxStackSize());
                    slots.add(j);
                }
                o.addProperty("slot_count", menu.slots.size());
                o.addProperty("first_player_slot", playerFirst);
                o.add("slots", slots);
            } catch (Exception | LinkageError e) {
                StackTraceElement at = null;
                for (StackTraceElement f : e.getStackTrace())
                    if (f.getClassName().startsWith("net.minecraft.world.inventory.") && !f.getClassName().contains("MenuType")) {
                        at = f;
                        break;
                    }
                o.addProperty("unavailable", e + (at == null ? "" : " at " + at.getClassName() + "." + at.getMethodName()));
            }
            menus.add(o);
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", SharedConstants.getCurrentVersion().name());
        root.add("menus", menus);
        JsonObject classes = new JsonObject();
        for (var e : slotClasses.entrySet()) classes.add(e.getKey(), e.getValue());
        root.add("slot_classes", classes);
        return root;
    }
}
