// mcgen gamedata, section "items": the code side of each item. Its data side - the default
// components - is Mojang's own report (`mcgen.py reports` -> reports/item_components/).

import com.google.gson.JsonArray;
import com.google.gson.JsonElement;
import com.google.gson.JsonObject;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.world.item.BlockItem;
import net.minecraft.world.item.Item;

final class ItemSection {
    private ItemSection() {}

    private static final Dump.Hooks HOOKS = new Dump.Hooks(Item.class);

    static JsonObject run(GameData.Env env) {
        JsonArray items = new JsonArray();
        for (Item it : BuiltInRegistries.ITEM) {
            JsonObject o = new JsonObject();
            o.addProperty("name", BlockSection.key(BuiltInRegistries.ITEM.getKey(it)));
            o.addProperty("id", BuiltInRegistries.ITEM.getId(it));
            o.add("class", Dump.chain(it.getClass(), Object.class));
            o.add("interfaces", Dump.interfaces(it.getClass()));
            if (it instanceof BlockItem bi) o.addProperty("block", BlockSection.key(BuiltInRegistries.BLOCK.getKey(bi.getBlock())));
            o.addProperty("enabled", it.isEnabled(env.features()));
            o.addProperty("description_id", it.getDescriptionId());
            JsonElement remainder = Dump.enc(it.getCraftingRemainder());
            if (remainder != null && !remainder.isJsonNull()) o.add("crafting_remainder", remainder);
            o.add("hooks", HOOKS.overridden(it.getClass()));
            o.add("fields", Dump.fields(it, Item.class));
            items.add(o);
        }
        JsonObject root = new JsonObject();
        root.addProperty("version", net.minecraft.SharedConstants.getCurrentVersion().name());
        root.add("hook_names", HOOKS.names());
        root.add("items", items);
        return root;
    }
}
