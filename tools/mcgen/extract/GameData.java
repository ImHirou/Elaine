// SPDX-License-Identifier: GPL-3.0-or-later
// mcgen gamedata: dump everything about the game that only exists once vanilla has loaded - what
// Extract.java (blockdata) and Mojang's reports do not cover. One JSON file per section:
//
//   states      per block state: every shape, sturdy faces, redstone/piston/path/heightmap predicates,
//               rotations and mirrors, the fluid state, POI and block entity type
//   blocks      per block: class chain, overridden hooks, properties and how they make a state id,
//               every field of the block object (its constructor arguments), sounds, offsets
//   fluids      fluids and fluid states
//   items       per item: class chain, overridden hooks, fields
//   entities    per entity type: class chain, dimensions, default attributes, spawn placement;
//               per entity class: its synchronized data ids and serializers
//   registries  every other static registry, each entry field by field
//   enums       every enum of the server jar: ordinals, ids, serialized names
//   statics     static tables and constants kept in code (waxing and weathering pairs, dispenser
//               behaviours, cauldron interactions, pose dimensions, level and entity event ids, ...)
//   menus       slot layout of each menu type, as far as a menu can be built without a player
//
// It loads the game the way a dedicated server does before it creates a world (the gametest
// server's recipe: Bootstrap, then WorldLoader.load over the vanilla data pack) - so tags and item
// components are bound and every predicate answers as it does in a running server. No world, no
// server object, no network. Field and hook names are vanilla's (unobfuscated since 26.1).
//
// Run through `tools/mcgen/mcgen.py gamedata` (which supplies the class path and Java version).

import com.google.gson.JsonArray;
import com.google.gson.JsonObject;
import java.nio.file.Files;
import java.nio.file.Path;
import java.util.ArrayList;
import java.util.Arrays;
import java.util.LinkedHashMap;
import java.util.List;
import java.util.Map;
import net.minecraft.SharedConstants;
import net.minecraft.commands.Commands;
import net.minecraft.core.LayeredRegistryAccess;
import net.minecraft.core.RegistryAccess;
import net.minecraft.resources.Identifier;
import net.minecraft.server.Bootstrap;
import net.minecraft.server.RegistryLayer;
import net.minecraft.server.ReloadableServerResources;
import net.minecraft.server.WorldLoader;
import net.minecraft.server.packs.repository.PackRepository;
import net.minecraft.server.packs.repository.ServerPacksSource;
import net.minecraft.server.permissions.LevelBasedPermissionSet;
import net.minecraft.util.Util;
import net.minecraft.world.flag.FeatureFlagSet;
import net.minecraft.world.flag.FeatureFlags;
import net.minecraft.world.level.DataPackConfig;
import net.minecraft.world.level.WorldDataConfiguration;

public final class GameData {
    /** What a section may use: the loaded game. */
    record Env(RegistryAccess.Frozen registries, ReloadableServerResources resources, FeatureFlagSet features) {}

    interface Section {
        JsonObject run(Env env) throws Exception;
    }

    static final Map<String, Section> SECTIONS = new LinkedHashMap<>();

    static {
        SECTIONS.put("states", StateSection::run);
        SECTIONS.put("blocks", BlockSection::run);
        SECTIONS.put("fluids", BlockSection::fluids);
        SECTIONS.put("items", ItemSection::run);
        SECTIONS.put("entities", EntitySection::run);
        SECTIONS.put("registries", RegistrySection::run);
        SECTIONS.put("enums", MiscSection::enums);
        SECTIONS.put("statics", MiscSection::statics);
        SECTIONS.put("menus", MiscSection::menus);
    }

    record Loaded(ReloadableServerResources resources, LayeredRegistryAccess<RegistryLayer> layers) {}

    static Env load() throws Exception {
        SharedConstants.tryDetectVersion();
        Bootstrap.bootStrap();
        // GameTestServer.create() without the server: the vanilla pack only, vanilla feature flags.
        PackRepository packs = ServerPacksSource.createVanillaTrustedRepository();
        packs.reload();
        FeatureFlagSet features = FeatureFlags.DEFAULT_FLAGS;
        WorldDataConfiguration data = new WorldDataConfiguration(new DataPackConfig(List.of("vanilla"), List.of()), features);
        WorldLoader.InitConfig init = new WorldLoader.InitConfig(new WorldLoader.PackConfig(packs, data, false, true),
            Commands.CommandSelection.DEDICATED, LevelBasedPermissionSet.OWNER);
        Loaded loaded = Util.blockUntilDone(executor -> WorldLoader.load(init,
            context -> new WorldLoader.DataLoadOutput<>(Boolean.TRUE, context.datapackDimensions()),
            (resourceManager, resources, layers, cookie) -> {
                resourceManager.close();
                return new Loaded(resources, layers);
            }, Util.backgroundExecutor(), executor)).get();
        return new Env(loaded.layers().compositeAccess(), loaded.resources(), features);
    }

    public static void main(String[] args) throws Exception {
        final java.io.PrintStream stdout = System.out;  // Bootstrap redirects System.out into the game log
        Path out = Path.of(args[0]);
        List<String> wanted = args.length > 1 ? Arrays.asList(args).subList(1, args.length) : new ArrayList<>(SECTIONS.keySet());
        for (String name : wanted)
            if (!SECTIONS.containsKey(name)) {
                System.err.println("gamedata: no section '" + name + "' (have: " + String.join(", ", SECTIONS.keySet()) + ")");
                System.exit(2);
            }
        Env env = load();
        Dump.index(env.registries());
        Files.createDirectories(out);

        JsonObject manifest = new JsonObject();
        manifest.addProperty("version", SharedConstants.getCurrentVersion().name());
        manifest.addProperty("protocol", SharedConstants.getProtocolVersion());
        JsonArray flags = new JsonArray();
        for (Identifier id : new java.util.TreeSet<>(FeatureFlags.REGISTRY.toNames(env.features()))) flags.add(id.toString());
        manifest.add("feature_flags", flags);
        JsonObject sections = new JsonObject();
        for (String name : wanted) {
            long t0 = System.nanoTime();
            int before = Dump.problems.size();
            JsonObject root;
            try {
                root = SECTIONS.get(name).run(env);
            } catch (Exception | LinkageError e) {
                e.printStackTrace();
                Dump.problem(name + ": " + e);
                continue;
            }
            Dump.write(out.resolve(name + ".json"), root);
            JsonObject counts = new JsonObject();
            for (Map.Entry<String, com.google.gson.JsonElement> e : root.entrySet())
                if (e.getValue().isJsonArray()) counts.addProperty(e.getKey(), e.getValue().getAsJsonArray().size());
                else if (e.getValue().isJsonObject()) counts.addProperty(e.getKey(), e.getValue().getAsJsonObject().size());
            sections.add(name, counts);
            StringBuilder line = new StringBuilder("gamedata: " + name + ":");
            for (Map.Entry<String, com.google.gson.JsonElement> e : counts.entrySet())
                if (counts.size() <= 8) line.append(' ').append(e.getValue()).append(' ').append(e.getKey().replace('_', ' ')).append(',');
            if (counts.size() > 8) line.append(' ').append(counts.size()).append(" tables,");
            stdout.println(line + " " + (System.nanoTime() - t0) / 1_000_000 + " ms"
                + (Dump.problems.size() > before ? ", " + (Dump.problems.size() - before) + " PROBLEMS" : ""));
        }
        // The manifest describes a whole run; a run of some sections leaves the old one alone.
        if (args.length == 1) {
            manifest.add("sections", sections);
            JsonArray problems = new JsonArray();
            for (String p : Dump.problems) problems.add(p);
            manifest.add("problems", problems);
            Dump.write(out.resolve("manifest.json"), manifest);
        }
        for (String p : Dump.problems) System.err.println("gamedata: PROBLEM: " + p);
        stdout.flush();
        // Bootstrap and the loader start non-daemon worker pools; nothing else is pending.
        System.exit(Dump.problems.isEmpty() ? 0 : 3);
    }
}
