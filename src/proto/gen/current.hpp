// SPDX-License-Identifier: GPL-3.0-or-later
// The protocol version this build speaks. This is the ONLY file that names a versioned
// directory: a version bump is `mcgen generate` + replacing v777 below (bump-mc-version skill).
#pragma once

#include "proto/gen/v777/blocks.hpp"
#include "proto/gen/v777/meta.hpp"
#include "proto/gen/v777/packet_ids.hpp"
#include "proto/gen/v777/registries.hpp"

#define PROTO_GEN_DIR "proto/gen/v777"
// Larger generated headers are included where they are used, through these macros:
#define PROTO_GEN_BLOBS_HPP "proto/gen/v777/blobs.hpp"                      // embedded packet bodies (proto/static_data.cpp only)
#define PROTO_GEN_BLOCK_DATA_HPP "proto/gen/v777/block_data.hpp"            // hot per-state tables (world/block.hpp)
#define PROTO_GEN_BLOCK_INFO_HPP "proto/gen/v777/block_info.hpp"            // layout of the cold per-block/item tables
#define PROTO_GEN_BLOCK_TABLES_HPP "proto/gen/v777/block_tables.hpp"        // the cold tables themselves (world/block.cpp only)
#define PROTO_GEN_ITEMS_HPP "proto/gen/v777/items.hpp"                      // item and data component ids by name
#define PROTO_GEN_DIMENSION_TYPES_HPP "proto/gen/v777/dimension_types.hpp"  // height / skylight per dimension type
#define PROTO_GEN_ENTITIES_HPP "proto/gen/v777/entities.hpp"                // entity types, entity-data ids, poses
#define PROTO_GEN_RECIPES_HPP "proto/gen/v777/recipes.hpp"        // crafting and smelting recipes (item/recipes.cpp only)
#define PROTO_GEN_STATIC_IDS_HPP "proto/gen/v777/static_ids.hpp"  // menus, block entity types, sounds, particles
#define PROTO_GEN_COMMAND_DATA_HPP \
    "proto/gen/v777/command_data.hpp"                             // names for commands: properties, items (world/block.cpp only)
#define PROTO_GEN_LANG_HPP "proto/gen/v777/lang.hpp"              // en_us messages for the console (text/lang.cpp only)
#define PROTO_GEN_CHAT_TYPES_HPP "proto/gen/v777/chat_types.hpp"  // how a chat message of each type reads (server/chat.cpp only)
#define PROTO_GEN_TIMELINES_HPP "proto/gen/v777/timelines.hpp"              // world clocks, timelines, time markers (server/clock.cpp only)
#define PROTO_GEN_PHYSICS_HPP "proto/gen/v777/physics.hpp"  // what entity physics reads off a block state and an entity type
#define PROTO_GEN_WORLDGEN_HPP "proto/gen/v777/worldgen.hpp"  // the overworld's generator: noises (src/worldgen only)

namespace proto {
namespace ids = gen::ids;
namespace reg = gen::reg;
}  // namespace proto
