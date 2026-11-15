// mcgen gamedata, section "registries": every static registry that has no section of its own,
// each entry as its id, class and fields. Attributes (default/min/max), mob effects (their attribute
// modifiers), potions, game rules, sound events (fixed range), POI types (their block states), block
// entity types (valid blocks), villager professions, stats, particles... - whatever the entry holds.
//
// Data-driven registries (enchantments, biomes, damage types, recipes, loot tables...) are not here:
// they are the JSON files under data/minecraft/ in the server jar, which gen.py reads directly.

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.util.Set;
import net.minecraft.core.Registry;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.resources.Identifier;

final class RegistrySection {
    private RegistrySection() {}

    private static final Set<String> OWN_SECTION = Set.of("minecraft:block", "minecraft:item", "minecraft:entity_type", "minecraft:fluid");

    static JsonObject run(GameData.Env env) {
        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        java.util.TreeMap<String, Registry<?>> sorted = new java.util.TreeMap<>();
        for (Registry<?> r : BuiltInRegistries.REGISTRY) sorted.put(r.key().identifier().toString(), r);
        for (java.util.Map.Entry<String, Registry<?>> e : sorted.entrySet())
            if (!OWN_SECTION.contains(e.getKey())) root.add(e.getKey(), entries(e.getValue()));
        return root;
    }

    private static <T> JsonArray entries(Registry<T> r) {
        JsonArray out = new JsonArray();
        for (T v : r) {
            JsonObject o = new JsonObject();
            o.addProperty("name", String.valueOf(r.getKey(v)));
            o.addProperty("id", r.getId(v));
            if (v instanceof Identifier || !Dump.vanilla(v.getClass()) || Dump.opaque(v)) {
                // A registry of plain values (custom_stat) or of codecs (the *_type registries): the name is all there is.
                if (v instanceof Identifier) o.addProperty("value", v.toString());
            } else {
                o.add("class", Dump.chain(v.getClass(), Object.class));
                JsonObject fields = Dump.fields(v);
                if (fields.size() > 0) o.add("fields", fields);
            }
            out.add(o);
        }
        return out;
    }
}
