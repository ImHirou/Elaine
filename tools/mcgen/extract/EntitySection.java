// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen gamedata, section "entities": entity types, and the classes behind them.
//
// "types" is one record per entity type; "classes" is one per Java class on the way from a type's
// class up to Entity, with the synchronized data that class declares. A type's set_entity_data
// layout is the concatenation of its chain, root first.
//
// An entity cannot be made here (it needs a level), so two things come from elsewhere: the default
// value of each synchronized datum is added to this file by gamedata.py, which reads the
// defineSynchedData() bytecode; dimensions in other poses are code
// (`mcgen.py pseudo <class> --method getDefaultDimensions`; for players, Avatar.POSES in statics.json).

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.lang.reflect.Field;
import java.lang.reflect.Modifier;
import java.lang.reflect.ParameterizedType;
import java.lang.reflect.Type;
import java.util.IdentityHashMap;
import java.util.Map;
import net.minecraft.core.Holder;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.network.syncher.EntityDataAccessor;
import net.minecraft.network.syncher.EntityDataSerializer;
import net.minecraft.network.syncher.EntityDataSerializers;
import net.minecraft.world.entity.Entity;
import net.minecraft.world.entity.EntityAttachments;
import net.minecraft.world.entity.EntityType;
import net.minecraft.world.entity.EntityTypes;
import net.minecraft.world.entity.LivingEntity;
import net.minecraft.world.entity.MobCategory;
import net.minecraft.world.entity.SpawnPlacementType;
import net.minecraft.world.entity.SpawnPlacementTypes;
import net.minecraft.world.entity.SpawnPlacements;
import net.minecraft.world.entity.ai.attributes.Attribute;
import net.minecraft.world.entity.ai.attributes.AttributeInstance;
import net.minecraft.world.entity.ai.attributes.AttributeSupplier;
import net.minecraft.world.entity.ai.attributes.DefaultAttributes;

final class EntitySection {
    private EntitySection() {}

    // EntityType fields that the record already has under its own keys.
    private static final String[] COVERED_FIELDS = {
        "category", "dimensions", "clientTrackingRange", "updateInterval", "trackDeltas", "fireImmune", "summon", "serialize",
        "canSpawnFarFromPlayer", "allowedInPeaceful", "descriptionId", "lootTable", "requiredFeatures",
    };

    /** EntityType -> its entity class: the type argument of the EntityTypes field that holds it. */
    static Map<EntityType<?>, Class<?>> entityClasses() {
        Map<EntityType<?>, Class<?>> out = new IdentityHashMap<>();
        for (Field f : EntityTypes.class.getDeclaredFields()) {
            if (!Modifier.isStatic(f.getModifiers()) || f.getType() != EntityType.class) continue;
            try {
                f.setAccessible(true);
                Type arg = ((ParameterizedType) f.getGenericType()).getActualTypeArguments()[0];
                Class<?> c = arg instanceof ParameterizedType p ? (Class<?>) p.getRawType() : arg instanceof Class<?> k ? k : null;
                if (c == null) Dump.problem("EntityTypes." + f.getName() + ": type argument " + arg + " is not a class");
                else out.put((EntityType<?>) f.get(null), c);
            } catch (ReflectiveOperationException | RuntimeException e) {
                Dump.problem("EntityTypes." + f.getName() + ": " + e);
            }
        }
        return out;
    }

    private static Map<Object, String> fieldNames(Class<?> holder, Class<?> type) {
        Map<Object, String> out = new IdentityHashMap<>();
        for (Field f : holder.getDeclaredFields())
            if (Modifier.isStatic(f.getModifiers()) && type.isAssignableFrom(f.getType()))
                try {
                    f.setAccessible(true);
                    out.put(f.get(null), f.getName());
                } catch (ReflectiveOperationException e) {
                    Dump.problem(holder.getName() + "." + f.getName() + ": " + e);
                }
        return out;
    }

    @SuppressWarnings("unchecked")
    private static JsonObject attributes(EntityType<?> type) {
        AttributeSupplier supplier = DefaultAttributes.getSupplier((EntityType<? extends LivingEntity>) type);
        Object instances = Dump.get(supplier, AttributeSupplier.class, "instances");
        Map<String, Double> sorted = new java.util.TreeMap<>();
        if (instances instanceof Map<?, ?> m)
            for (Map.Entry<?, ?> e : m.entrySet())
                sorted.put(((Holder<Attribute>) e.getKey()).getRegisteredName(), ((AttributeInstance) e.getValue()).getBaseValue());
        JsonObject out = new JsonObject();
        for (Map.Entry<String, Double> e : sorted.entrySet()) out.addProperty(e.getKey(), e.getValue());
        return out;
    }

    static JsonObject run(GameData.Env env) throws Exception {
        Map<EntityType<?>, Class<?>> classOf = entityClasses();
        Map<Object, String> serializerNames = fieldNames(EntityDataSerializers.class, EntityDataSerializer.class);
        Map<Object, String> placementNames = fieldNames(SpawnPlacementTypes.class, SpawnPlacementType.class);

        String[] byId = new String[serializerNames.size()];
        for (Map.Entry<Object, String> e : serializerNames.entrySet()) {
            int id = EntityDataSerializers.getSerializedId((EntityDataSerializer<?>) e.getKey());
            if (id >= 0 && id < byId.length && byId[id] == null) byId[id] = e.getValue();
            else Dump.problem("EntityDataSerializers." + e.getValue() + ": id " + id + " is not registered once");
        }
        JsonArray serializers = new JsonArray();
        for (String name : byId) serializers.add(String.valueOf(name));

        JsonArray categories = new JsonArray();
        for (MobCategory c : MobCategory.values()) {
            JsonObject o = new JsonObject();
            o.addProperty("name", c.getName());
            o.addProperty("enum", c.name());
            o.addProperty("max_per_chunk", c.getMaxInstancesPerChunk());
            o.addProperty("friendly", c.isFriendly());
            o.addProperty("persistent", c.isPersistent());
            o.addProperty("despawn_distance", c.getDespawnDistance());
            o.addProperty("no_despawn_distance", c.getNoDespawnDistance());
            categories.add(o);
        }

        Map<Class<?>, JsonObject> classes = new java.util.TreeMap<>((a, b) -> a.getName().compareTo(b.getName()));
        JsonArray types = new JsonArray();
        for (EntityType<?> t : BuiltInRegistries.ENTITY_TYPE) {
            String name = BlockSection.key(BuiltInRegistries.ENTITY_TYPE.getKey(t));
            JsonObject o = new JsonObject();
            o.addProperty("name", name);
            o.addProperty("id", BuiltInRegistries.ENTITY_TYPE.getId(t));
            Class<?> c = classOf.get(t);
            if (c == null) {
                Dump.problem(name + ": no EntityTypes field holds it");
            } else {
                o.add("class", Dump.chain(c, Object.class));
                o.add("interfaces", Dump.interfaces(c));
                for (Class<?> k = c; k != null && Entity.class.isAssignableFrom(k); k = k.getSuperclass())
                    if (!classes.containsKey(k)) classes.put(k, entityClass(k, serializerNames));
            }
            o.addProperty("category", t.getCategory().getName());
            o.addProperty("width", t.getDimensions().width());
            o.addProperty("height", t.getDimensions().height());
            o.addProperty("eye_height", t.getDimensions().eyeHeight());
            o.addProperty("fixed_size", t.getDimensions().fixed());
            EntityAttachments attachments = t.getDimensions().attachments();
            o.add("attachments", Dump.enc(Dump.get(attachments, EntityAttachments.class, "attachments")));
            o.addProperty("tracking_range", t.clientTrackingRange());
            o.addProperty("update_interval", t.hasUpdateInterval() ? t.updateInterval() : 0);
            o.addProperty("track_deltas", t.trackDeltas());
            o.addProperty("fire_immune", t.fireImmune());
            o.addProperty("summon", t.canSummon());
            o.addProperty("serialize", t.canSerialize());
            o.addProperty("spawn_far_from_player", t.canSpawnFarFromPlayer());
            o.addProperty("allowed_in_peaceful", t.isAllowedInPeaceful());
            o.addProperty("enabled", t.isEnabled(env.features()));
            o.addProperty("description_id", t.getDescriptionId());
            if (t.getDefaultLootTable().isPresent())
                o.addProperty("loot_table", BlockSection.key(t.getDefaultLootTable().get().identifier()));
            SpawnPlacementType placement = SpawnPlacements.getPlacementType(t);
            o.addProperty("spawn_placement", placementNames.getOrDefault(placement, "?" + Dump.simple(placement.getClass())));
            o.addProperty("spawn_heightmap", SpawnPlacements.getHeightmapType(t).name());
            if (DefaultAttributes.hasSupplier(t)) o.add("attributes", attributes(t));
            JsonObject fields = Dump.fields(t);
            for (String covered : COVERED_FIELDS) fields.remove(covered);
            o.add("fields", fields);
            types.add(o);
        }

        JsonObject byClass = new JsonObject();
        for (Map.Entry<Class<?>, JsonObject> e : classes.entrySet()) byClass.add(e.getKey().getName(), e.getValue());
        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        root.add("serializers", serializers);  // index = the serializer id set_entity_data carries
        root.add("categories", categories);
        root.add("types", types);
        root.add("classes", byClass);
        return root;
    }

    /** Synchronized data declared by one class: ids continue from the superclass (ClassTreeIdRegistry). */
    private static JsonObject entityClass(Class<?> k, Map<Object, String> serializerNames) {
        JsonObject o = new JsonObject();
        if (k != Entity.class) o.addProperty("super", k.getSuperclass().getName());
        o.addProperty("abstract", Modifier.isAbstract(k.getModifiers()));
        Map<Integer, JsonObject> byId = new java.util.TreeMap<>();
        for (Field f : k.getDeclaredFields()) {
            if (!Modifier.isStatic(f.getModifiers()) || f.getType() != EntityDataAccessor.class) continue;
            try {
                f.setAccessible(true);
                EntityDataAccessor<?> a = (EntityDataAccessor<?>) f.get(null);
                JsonObject d = new JsonObject();
                d.addProperty("id", a.id());
                d.addProperty("field", f.getName());
                String ser = serializerNames.get(a.serializer());
                if (ser == null) Dump.problem(k.getName() + "." + f.getName() + ": unknown serializer");
                d.addProperty("serializer", String.valueOf(ser));
                d.addProperty("serializer_id", EntityDataSerializers.getSerializedId(a.serializer()));
                // "EntityDataAccessor<java.util.Optional<net.minecraft.core.BlockPos>>" -> "Optional<BlockPos>"
                String type = f.getGenericType().getTypeName();
                d.addProperty("type", type.replaceAll("^[^<]*<(.*)>$", "$1").replaceAll("\\b[a-z][\\w]*\\.", ""));
                if (byId.put(a.id(), d) != null) Dump.problem(k.getName() + ": two accessors with id " + a.id());
            } catch (ReflectiveOperationException | LinkageError e) {
                Dump.problem(k.getName() + "." + f.getName() + ": " + e);
            }
        }
        JsonArray data = new JsonArray();
        for (JsonObject d : byId.values()) data.add(d);
        o.add("data", data);
        return o;
    }
}
