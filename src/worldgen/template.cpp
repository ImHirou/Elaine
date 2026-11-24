// SPDX-License-Identifier: GPL-3.0-or-later
// Structure templates as features place them: TemplateFeature (the desert's well, a sulfur spring)
// and FossilFeature, and what they use of StructureTemplate.placeInWorld - one palette, a rotation,
// no mirror and no entities; the processors block_rot, protected_blocks and rule; a block entity's
// tag where a block carries one; water that stays in what is put into it; and the block updates at
// the end, at the template's edge and of every block it set.
//
// The templates are tables (gen_templates.py): the jar's .nbt as the game holds them once they
// are loaded, their blocks in the order StructureTemplate keeps them - which is the order they are
// processed and set in, draw by draw.
#include <algorithm>

#include "nbt/nbt.hpp"
#include "worldgen/feature.hpp"
#include "worldgen/props.hpp"
#include "worldgen/shape.hpp"

namespace worldgen {

namespace {

namespace blk = proto::gen::block;

// Rotation, by its ordinal.
enum : u32 { kNone, kClockwise90, kClockwise180, kCounterclockwise90 };

// StructureTemplate.transform with no mirror, about the origin.
Pos turned(Pos p, u32 rotation) {
    switch (rotation) {
        case kClockwise90: return {-p.z, p.y, p.x};
        case kClockwise180: return {-p.x, p.y, -p.z};
        case kCounterclockwise90: return {p.z, p.y, -p.x};
        default: return p;
    }
}

// BlockState.rotate: most states are the same whichever way they are turned.
u16 turned(u16 state, u32 rotation) {
    if (rotation == kNone) return state;
    const ft::StateRotation* const end = ft::kStateRotations + ft::kStateRotationCount;
    const ft::StateRotation* const row =
        std::lower_bound(ft::kStateRotations, end, state, [](const ft::StateRotation& r, u16 s) { return r.state < s; });
    return row != end && row->state == state ? row->to[rotation - 1] : state;
}

// StructurePlaceSettings, as the two features make them.
struct Settings {
    u32 rotation;
    u32 processors, processor_count;
    bool boxed;          // FossilFeature: nothing beyond the chunks around the one it is in...
    i32 x0, z0, x1, z1;  // ...at any height of the world
    u32 flags;           // what a block is set with
    bool inside(Pos p) const {
        return !boxed || (p.x >= x0 && p.x <= x1 && p.z >= z0 && p.z <= z1 && !Region::outside_height(p.y));
    }
};

struct Placer {
    Region& region;
    FeatureWork& work;
    FeatureRandom& random;

    // StructureProcessor.processBlock, one after the other: what becomes of a block, or nothing.
    bool process(FeatureWork::TemplateBlock& b, const Settings& s) {
        for (u32 i = s.processors; i < s.processors + s.processor_count; ++i) {
            const ft::Processor& p = ft::kProcessors[i];
            switch (p.op) {
                case ft::ProcessorOp::kBlockRot: {
                    if (p.set != 0 && !in_state_set(p.set - 1u, b.state)) break;
                    const float chance = random.next_float();
                    if (chance > p.integrity) return false;
                    break;
                }
                case ft::ProcessorOp::kProtectedBlocks:
                    if (in_state_set(p.set, region.block(b.pos.x, b.pos.y, b.pos.z))) return false;
                    break;
                case ft::ProcessorOp::kRule:
                    for (u32 r = p.rules; r < p.rules + u32{p.rule_count}; ++r) {
                        const ft::ProcessorRule& rule = ft::kProcessorRules[r];
                        if (!test(ft::kRules[rule.input], b.state, b.pos.y)) continue;
                        if (!test(ft::kRules[rule.location], region.block(b.pos.x, b.pos.y, b.pos.z), b.pos.y)) continue;
                        b.state = rule.output_state;
                        if (rule.loot_table != 0) {
                            // AppendLoot: a seed off a source of the block's own (RuleProcessor makes one a
                            // block, of its place: nothing else here draws from it).
                            LegacyRandom own(position_seed(b.pos.x, b.pos.y, b.pos.z));
                            b.loot = rule.loot_table;
                            b.loot_seed = own.next_long();
                            b.tagged = true;
                        }
                        break;
                    }
                    break;
            }
        }
        return true;
    }

    // SimpleWaterloggedBlock.placeLiquid: water handed to a block that can hold it.
    void place_liquid(Pos p, u16 state, u16 fluid_of) {
        if (!in_state_set(ft::kSetTakesWater, state) || !in_state_set(ft::kSetWaterSource, fluid_of)) return;
        region.set_block(p.x, p.y, p.z, prop::with_flag(state, ft::Prop::kWaterlogged, true), 3);
        region.schedule_tick(p.x, p.y, p.z, world::fluid::kWater, true);
    }

    // What the block entity of a template's block saves once it has read the block's tag
    // (BlockEntity.loadWithComponents, then saveWithFullMetadata).
    void write_block_entity(const FeatureWork::TemplateBlock& b) {
        if (!region.block_entity(b.pos.x, b.pos.y, b.pos.z)) return;  // (the state has none)
        base::ByteBuf& tag = work.tag;
        tag.len = 0;
        nbt::Writer w(tag);
        w.root(nbt::kCompound);
        // BrushableBlockEntity: a loot table, and its seed unless that is nothing.
        if (b.loot != 0) {
            w.put_string("LootTable", ft::kLootTables[b.loot]);
            if (b.loot_seed != 0) w.put_long("LootTableSeed", b.loot_seed);
        }
        end_block_entity(w, "minecraft:brushable_block", b.pos);
        region.set_block_entity_tag(b.pos.x, b.pos.y, b.pos.z, {tag.data, tag.len});
    }

    // StructureTemplate.placeInWorld
    bool place(const ft::Template& t, Pos position, const Settings& s) {
        // StructurePlaceSettings.getRandomPalette: there is one, and a draw for it all the same.
        random.next_int(1);
        if (t.count == 0) return false;
        // processBlockInfos: every block of the template through the processors, before one is set.
        std::vector<FeatureWork::TemplateBlock>& blocks = work.template_blocks;
        blocks.clear();
        for (u32 i = t.first; i < t.first + t.count; ++i) {
            const ft::TemplateBlock& made = ft::kTemplateBlocks[i];
            const Pos turned_to = turned(Pos{made.x, made.y, made.z}, s.rotation);
            FeatureWork::TemplateBlock b{
                {position.x + turned_to.x, position.y + turned_to.y, position.z + turned_to.z}, made.state, made.tag != 0, 0, 0};
            if (!s.inside(b.pos)) continue;
            if (process(b, s)) blocks.push_back(b);
        }
        std::vector<Pos>& placed = work.template_placed;
        std::vector<Pos>& to_fill = work.template_fill;
        JavaHashSet& locked = work.template_locked;
        placed.clear();
        to_fill.clear();
        locked.clear();
        ShapeBox box{0, 0, 0, 0, 0, 0};
        for (const FeatureWork::TemplateBlock& b : blocks) {
            const Pos p = b.pos;
            const u16 before = region.block(p.x, p.y, p.z);  // (its fluid: what the place held)
            const u16 state = turned(b.state, s.rotation);
            // A block with a tag takes the place of a barrier: whatever block entity was there is gone.
            if (b.tagged) region.set_block(p.x, p.y, p.z, static_cast<u16>(blk::kBarrier), 820);
            if (!region.set_block(p.x, p.y, p.z, state, s.flags)) continue;
            if (placed.empty()) {
                box = {p.x, p.y, p.z, p.x, p.y, p.z};
            } else {
                box.x0 = std::min(box.x0, p.x), box.x1 = std::max(box.x1, p.x);
                box.y0 = std::min(box.y0, p.y), box.y1 = std::max(box.y1, p.y);
                box.z0 = std::min(box.z0, p.z), box.z1 = std::max(box.z1, p.z);
            }
            placed.push_back(p);
            if (b.tagged) write_block_entity(b);
            // Water stays in what is put into it: a block that holds a source of its own keeps it (and
            // gives none away below), one that can take water takes what was there.
            if (in_state_set(ft::kSetFluidSource, state)) {
                locked.add(p.x, p.y, p.z);
            } else if (in_state_set(ft::kSetLiquidContainer, state)) {
                place_liquid(p, state, before);
                if (!in_state_set(ft::kSetFluidSource, before)) to_fill.push_back(p);
            }
        }
        // What took no water takes it from a source above or beside it that the template did not bring.
        constexpr u32 kFrom[5] = {world::kUp, world::kNorth, world::kEast, world::kSouth, world::kWest};
        for (bool filled = true; filled && !to_fill.empty();) {
            filled = false;
            for (usize i = 0; i < to_fill.size();) {
                const Pos p = to_fill[i];
                u16 fluid_of = region.block(p.x, p.y, p.z);
                for (u32 d = 0; d < 5 && !in_state_set(ft::kSetFluidSource, fluid_of); ++d) {
                    const Pos n{p.x + world::kStepX[kFrom[d]], p.y + world::kStepY[kFrom[d]], p.z + world::kStepZ[kFrom[d]]};
                    const u16 beside = region.block(n.x, n.y, n.z);
                    if (in_state_set(ft::kSetFluidSource, beside) && !locked.contains(n.x, n.y, n.z)) fluid_of = beside;
                }
                if (in_state_set(ft::kSetFluidSource, fluid_of)) {
                    const u16 state = region.block(p.x, p.y, p.z);
                    if (in_state_set(ft::kSetLiquidContainer, state)) {
                        place_liquid(p, state, fluid_of);
                        filled = true;
                        to_fill.erase(to_fill.begin() + static_cast<std::ptrdiff_t>(i));
                        continue;
                    }
                }
                ++i;
            }
        }
        if (placed.empty()) return true;
        // The blocks at the template's edge and their neighbours are told of each other, then every
        // block that was set of all six of its own.
        work.shape.assign((box.cells() + 63) / 64, 0);
        ShapeBits shape{work.shape, box};
        for (const Pos p : placed) shape.fill(p.x, p.y, p.z);
        update_shape_at_edge(region, shape, s.flags & ~1u);
        u64 unported = 0;
        for (const Pos p : placed) {
            const u16 state = region.block(p.x, p.y, p.z);
            const u16 made = update_from_neighbour_shapes(region, state, p.x, p.y, p.z, unported);
            if (made != state) region.set_block(p.x, p.y, p.z, made, (s.flags & ~1u) | 16);
        }
        if (unported) region.note_unported_shapes(unported);
        return true;
    }
};

}  // namespace

// TemplateFeature: one of its templates, turned one of four ways, with the middle of its floor at
// the origin.
bool Decorator::place_template(const ft::TemplateFeature& feature, FeatureRandom& random, Pos origin) {
    const u32 id = pick_weighted(ft::kTemplateEntries, feature.entries, feature.entry_count, feature.total_weight, random);
    const u32 rotation = static_cast<u32>(random.next_int(4));  // Util.getRandom(entry.rotations()): all four, by ordinal
    const ft::Template& t = ft::kTemplates[id];
    // getRotatedOffset: half the template's size to the west and half to the north, as the template is turned.
    const Pos offset = turned(Pos{-(t.size_x / 2), 0, -(t.size_z / 2)}, rotation);
    const Settings settings{rotation, feature.processors, feature.processor_count, false, 0, 0, 0, 0, 3};
    Placer placer{region_, work_, random};
    return placer.place(t, {origin.x + offset.x, origin.y, origin.z + offset.z}, settings);
}

// FossilFeature: a skull or a spine of bone under the ground, with ore laid over it - where no more
// than so many corners of it would hang in the open.
bool Decorator::place_fossil(const ft::Fossil& fossil, FeatureRandom& random, Pos origin) {
    Region& region = region_;
    const u32 rotation = static_cast<u32>(random.next_int(4));  // Rotation.getRandom
    const i32 index = random.next_int(static_cast<i32>(fossil.count));
    const ft::FossilTemplates& pair = ft::kFossilTemplates[fossil.structures + static_cast<u32>(index)];
    const ft::Template& base = ft::kTemplates[pair.fossil];
    const ft::Template& overlay = ft::kTemplates[pair.overlay];
    const i32 chunk_x = origin.x >> 4, chunk_z = origin.z >> 4;
    Settings settings{rotation, 0, 0, true, chunk_x * 16 - 16, chunk_z * 16 - 16, chunk_x * 16 + 15 + 16, chunk_z * 16 + 15 + 16,
                      260};
    // StructureTemplate.getSize(rotation)
    const bool quarter = rotation == kClockwise90 || rotation == kCounterclockwise90;
    const i32 size_x = quarter ? base.size_z : base.size_x, size_z = quarter ? base.size_x : base.size_z;
    const Pos low{origin.x - size_x / 2, origin.y, origin.z - size_z / 2};
    i32 lowest = origin.y;
    for (i32 x = 0; x < size_x; ++x)
        for (i32 z = 0; z < size_z; ++z)
            lowest = std::min(lowest, region.height(world::heightmap::kOceanFloorWg, low.x + x, low.z + z));
    const i32 depth = random.next_int(10);
    const i32 target_y = std::max(lowest - 15 - depth, Region::kMinY + 10);
    // getZeroPositionWithTransform: where the template's own corner comes to lie once it is turned.
    Pos target{low.x, target_y, low.z};
    switch (rotation) {
        case kClockwise90: target.x += base.size_z - 1; break;
        case kClockwise180: target.x += base.size_x - 1, target.z += base.size_z - 1; break;
        case kCounterclockwise90: target.z += base.size_x - 1; break;
        default: break;
    }
    // countEmptyCorners, of the template's box (getBoundingBox): air, water or lava at a corner.
    const Pos far = turned(Pos{base.size_x - 1, base.size_y - 1, base.size_z - 1}, rotation);
    const i32 xs[2] = {target.x + std::min(0, far.x), target.x + std::max(0, far.x)};
    const i32 ys[2] = {target.y, target.y + far.y};
    const i32 zs[2] = {target.z + std::min(0, far.z), target.z + std::max(0, far.z)};
    i32 empty = 0;
    for (const i32 x : xs)
        for (const i32 y : ys)
            for (const i32 z : zs) {
                const u16 state = region.block(x, y, z);
                const u32 block = world::block_of(state);
                empty += world::is_air(state) || block == world::block_of(blk::kLava) || block == world::block_of(blk::kWater);
            }
    if (empty > fossil.max_empty_corners_allowed) return false;
    Placer placer{region, work_, random};
    settings.processors = fossil.fossil_processors, settings.processor_count = fossil.fossil_processor_count;
    placer.place(base, target, settings);
    settings.processors = fossil.overlay_processors, settings.processor_count = fossil.overlay_processor_count;
    placer.place(overlay, target, settings);
    return true;
}

}  // namespace worldgen
