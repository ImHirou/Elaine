#include "world/block.hpp"

#include PROTO_GEN_BLOCK_TABLES_HPP
#include PROTO_GEN_COMMAND_DATA_HPP
#include PROTO_GEN_ENTITIES_HPP

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

// ---- by name

namespace {
namespace cmd = gen::command;
static_assert(std::size(cmd::kBlockPropertyFirst) == state::kBlockCount + 1);
static_assert(std::size(cmd::kBlocksByName) == state::kBlockCount && std::size(cmd::kItems) == std::size(gen::kItems));
static_assert(std::size(cmd::kEntityTypesByName) == gen::entity_type::kCount);

// `order`: ids sorted by the name `name_of` gives them.
template <usize N, class NameOf>
u32 find_by_name(const unsigned short (&order)[N], std::string_view name, NameOf&& name_of) {
    usize lo = 0, hi = N;
    while (lo < hi) {
        const usize mid = (lo + hi) / 2;
        if (name_of(order[mid]) < name) {
            lo = mid + 1;
        } else {
            hi = mid;
        }
    }
    return lo < N && name_of(order[lo]) == name ? order[lo] : kNoId;
}
}  // namespace

u32 find_block(std::string_view name) {
    return find_by_name(cmd::kBlocksByName, name, [](u32 i) { return gen::kBlockNames[i]; });
}
u32 find_item(std::string_view name) {
    return find_by_name(cmd::kItemsByName, name, [](u32 i) { return cmd::kItems[i].name; });
}
u32 find_entity_type(std::string_view name) {
    return find_by_name(cmd::kEntityTypesByName, name, [](u32 i) { return gen::entity_type::kNames[i]; });
}
std::string_view item_name(u32 item) { return item < std::size(cmd::kItems) ? cmd::kItems[item].name : "?"; }
std::string_view item_text_key(u32 item) { return item < std::size(cmd::kItems) ? cmd::kItems[item].key : "?"; }
u32 item_rarity(u32 item) { return item < std::size(cmd::kItems) ? cmd::kItems[item].rarity : 0; }
std::string_view entity_type_name(u32 type) { return type < gen::entity_type::kCount ? gen::entity_type::kNames[type] : "?"; }
bool summonable(u32 type) { return type < gen::entity_type::kCount && cmd::kSummonable[type]; }

u32 block_property_count(u32 block) {
    DCHECK(block < state::kBlockCount);
    return static_cast<u32>(cmd::kBlockPropertyFirst[block + 1] - cmd::kBlockPropertyFirst[block]);
}
BlockProperty block_property(u32 block, u32 index) {
    const cmd::BlockProperty& use = cmd::kBlockProperties[cmd::kBlockPropertyFirst[block] + index];
    const cmd::Property& p = cmd::kProperties[use.property];
    return {p.name, {cmd::kPropertyValues + p.first_value, p.values}, use.stride};
}

std::span<const Box> collision_boxes(BlockState s, bool& dynamic) {
    const u32 id = state::collision_of(s);
    dynamic = id == state::kDynamicCollision;
    if (dynamic) return {};
    return {gen::kCollisionBoxes + gen::kCollisionFirst[id], gen::kCollisionBoxes + gen::kCollisionFirst[id + 1]};
}

}  // namespace world
