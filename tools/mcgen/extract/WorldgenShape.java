// SPDX-License-Identifier: GPL-3.0-or-later
// BlockState.updateShape, for the golden script: the rules src/worldgen/shape.hpp has of it, each held to
// the method itself in worlds drawn at random.
//
// Which states: shape_states.txt, which `mcgen.py generate` writes - a few states of every block whose
// class has a rule. Of each state so many worlds - the blocks within one of the place and two above and
// below it: a state of the same block, one of a few that a rule is likely to tell apart, or any state at
// all - and a direction; the neighbour is the block the world has there. What comes of it - the state, and
// which ticks were asked for (of a block or a fluid; a tick asked twice is one) - is folded into a number a
// state. A state whose method asks what such a world cannot say (lava, how fast it flows here) is marked
// and left out.

import java.lang.reflect.InvocationHandler;
import java.lang.reflect.Method;
import java.util.ArrayList;
import java.util.List;
import java.util.TreeSet;
import net.minecraft.core.BlockPos;
import net.minecraft.core.Direction;
import net.minecraft.core.registries.BuiltInRegistries;
import net.minecraft.util.RandomSource;
import net.minecraft.world.level.ScheduledTickAccess;
import net.minecraft.world.level.block.Block;
import net.minecraft.world.level.block.Blocks;
import net.minecraft.world.level.block.state.BlockState;
import net.minecraft.world.level.block.state.properties.BlockStateProperties;
import net.minecraft.world.level.material.Fluid;

final class WorldgenShape {
    private WorldgenShape() {}

    /** Whoever is asked for a tick writes it down: a fluid's as 1 << 20 | its id, a block's as its id. */
    static final class Ticks implements InvocationHandler {
        final TreeSet<Integer> asked = new TreeSet<>();
        boolean elsewhere;
        final ScheduledTickAccess access = (ScheduledTickAccess) java.lang.reflect.Proxy.newProxyInstance(
            WorldgenShape.class.getClassLoader(), new Class<?>[] {ScheduledTickAccess.class}, this);

        @Override
        public Object invoke(Object proxy, Method method, Object[] args) throws Throwable {
            if (method.getName().equals("scheduleTick") && args.length >= 3) {
                if (!WorldgenSurvive.AT.equals(args[0])) elsewhere = true;
                if (args[1] instanceof Fluid fluid) asked.add(1 << 20 | BuiltInRegistries.FLUID.getId(fluid));
                else asked.add(BuiltInRegistries.BLOCK.getId((Block) args[1]));
                return null;
            }
            if (method.getDeclaringClass() == Object.class) return method.invoke(this, args);
            throw new UnsupportedOperationException(method.getName());
        }
    }

    static void golden(WorldgenGolden.Out o) throws Exception {
        int states = Block.BLOCK_STATE_REGISTRY.size();
        List<BlockState> telling = new ArrayList<>(List.of(
            Blocks.AIR.defaultBlockState(), Blocks.CAVE_AIR.defaultBlockState(), Blocks.WATER.defaultBlockState(),
            Blocks.WATER.defaultBlockState().setValue(BlockStateProperties.LEVEL, 3), Blocks.LAVA.defaultBlockState(), Blocks.STONE.defaultBlockState(),
            Blocks.DIRT.defaultBlockState(), Blocks.GRASS_BLOCK.defaultBlockState(), Blocks.SAND.defaultBlockState(), Blocks.SNOW.defaultBlockState(),
            Blocks.SNOW_BLOCK.defaultBlockState(), Blocks.OAK_LOG.defaultBlockState(), Blocks.JUNGLE_LOG.defaultBlockState(),
            Blocks.MANGROVE_ROOTS.defaultBlockState(), Blocks.MUD.defaultBlockState(), Blocks.MAGMA_BLOCK.defaultBlockState(),
            Blocks.SOUL_SAND.defaultBlockState(), Blocks.BAMBOO.defaultBlockState(), Blocks.BAMBOO.defaultBlockState().setValue(BlockStateProperties.AGE_1, 1),
            Blocks.PALE_HANGING_MOSS.defaultBlockState(), Blocks.RED_MUSHROOM_BLOCK.defaultBlockState(), Blocks.BROWN_MUSHROOM_BLOCK.defaultBlockState(),
            Blocks.MUSHROOM_STEM.defaultBlockState(), Blocks.MOSS_BLOCK.defaultBlockState(), Blocks.CLAY.defaultBlockState(),
            Blocks.PODZOL.defaultBlockState(), Blocks.GLASS.defaultBlockState(), Blocks.OAK_SLAB.defaultBlockState()));
        for (int distance : new int[] {1, 2, 6, 7}) telling.add(Blocks.OAK_LEAVES.defaultBlockState().setValue(BlockStateProperties.DISTANCE, distance));
        telling.add(Blocks.MANGROVE_LEAVES.defaultBlockState().setValue(BlockStateProperties.WATERLOGGED, true));
        for (var side : new net.minecraft.world.level.block.state.properties.BooleanProperty[] {BlockStateProperties.UP, BlockStateProperties.NORTH,
                                                                                                   BlockStateProperties.EAST, BlockStateProperties.SOUTH,
                                                                                                   BlockStateProperties.WEST})
            telling.add(Blocks.VINE.defaultBlockState().setValue(side, true));
        o.i32(telling.size());
        for (BlockState state : telling) o.i32(Block.getId(state));
        java.nio.file.Path list = WorldgenRef.out.resolve("shape_states.txt");
        if (!java.nio.file.Files.exists(list)) {
            WorldgenRef.problem("no " + list + ": `mcgen.py generate` writes which states' updateShape src/worldgen has a rule for");
            return;
        }
        List<Integer> ids = new ArrayList<>();
        for (String line : java.nio.file.Files.readAllLines(list))
            if (!line.isBlank()) ids.add(Integer.parseInt(line.trim()));
        final int trials = 300;
        o.i32(ids.size());
        o.i32(trials);
        int left = 0;
        for (int id : ids) {
            BlockState state = Block.stateById(id);
            int first = Block.getId(state.getBlock().getStateDefinition().getPossibleStates().getFirst());
            int count = state.getBlock().getStateDefinition().getPossibleStates().size();
            long fold = 0xcbf29ce484222325L;
            boolean said = true;
            WorldgenSurvive.Probe probe = new WorldgenSurvive.Probe();
            for (int trial = 0; trial < trials && said; ++trial) {
                WorldgenSurvive.Mix mix = new WorldgenSurvive.Mix((long) id * 1000003L + trial);
                java.util.function.Supplier<BlockState> pick = () -> {
                    long r = mix.next();
                    int kind = (int) (r & 3);
                    long n = r >>> 2;
                    return kind == 0 ? Block.stateById(first + (int) (n % count)) : kind == 3 ? Block.stateById((int) (n % states)) : telling.get((int) (n % telling.size()));
                };
                probe.light = (mix.next() & 1) != 0 ? 15 : 0;
                probe.rest = pick.get();
                probe.blocks.clear();
                for (int dx = -1; dx <= 1; ++dx)
                    for (int dy = -2; dy <= 2; ++dy)
                        for (int dz = -1; dz <= 1; ++dz) probe.blocks.put(WorldgenSurvive.AT.offset(dx, dy, dz), pick.get());
                probe.blocks.put(WorldgenSurvive.AT, state);
                Direction direction = Direction.values()[(int) ((mix.next() >>> 1) % 6)];
                BlockPos beside = WorldgenSurvive.AT.relative(direction);
                Ticks ticks = new Ticks();
                BlockState made;
                try {
                    made = state.updateShape(probe.level, ticks.access, WorldgenSurvive.AT, direction, beside, probe.blocks.get(beside), RandomSource.create(0L));
                } catch (RuntimeException | Error e) {
                    said = false;
                    break;
                }
                if (ticks.elsewhere) {
                    WorldgenRef.problem(state + " asks for a tick somewhere else than where it stands");
                    return;
                }
                fold = WorldgenGolden.fnv(fold, Block.getId(made));
                fold = WorldgenGolden.fnv(fold, ticks.asked.size());
                for (int tick : ticks.asked) fold = WorldgenGolden.fnv(fold, tick);
            }
            o.i32(id);
            o.u8(said ? 1 : 0);
            o.i64(fold);
            if (!said) ++left;
        }
        System.err.println("worldgen: golden features: " + ids.size() + " block states told of a neighbour (updateShape), " + trials + " worlds each; " + left
            + " left out (they ask what such a world cannot say)");
    }
}
