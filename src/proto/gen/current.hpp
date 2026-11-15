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

namespace proto {
namespace ids = gen::ids;
namespace reg = gen::reg;
}  // namespace proto
