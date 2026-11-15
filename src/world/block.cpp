#include "world/block.hpp"

#include PROTO_GEN_BLOCK_TABLES_HPP

namespace world {

namespace gen = proto::gen;

static_assert(std::size(gen::kBlocks) == state::kBlockCount);
static_assert(std::size(gen::kCollisionFirst) == state::kCollisionShapes + 1);
// kStepX/Y/Z and opposite() assume this order (a pair of opposite directions differs in bit 0).
static_assert(kDown == 0 && kUp == 1 && kNorth == 2 && kSouth == 3 && kWest == 4 && kEast == 5);

u32 block_of(BlockState s) { return state::block_of(s); }

const BlockInfo& block_info(u32 block) {
    DCHECK(block < std::size(gen::kBlocks));
    return gen::kBlocks[block];
}

std::string_view block_name(u32 block) { return block < std::size(gen::kBlockNames) ? gen::kBlockNames[block] : "?"; }

u32 item_count() { return static_cast<u32>(std::size(gen::kItems)); }

const ItemInfo& item_info(u32 item) {
    DCHECK(item < std::size(gen::kItems));
    return gen::kItems[item];
}

std::span<const Box> collision_boxes(BlockState s, bool& dynamic) {
    const u32 id = state::collision_of(s);
    dynamic = id == state::kDynamicCollision;
    if (dynamic) return {};
    return {gen::kCollisionBoxes + gen::kCollisionFirst[id], gen::kCollisionBoxes + gen::kCollisionFirst[id + 1]};
}

}  // namespace world
