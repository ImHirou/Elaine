// SPDX-License-Identifier: GPL-3.0-or-later
// The order of a java.util.HashSet<BlockPos>.
//
// Vanilla's trees keep the blocks they set in hash sets and then walk them: the logs and the leaves
// a decorator is handed are a set copied into a list (TreeDecorator.Context), with a draw for every
// one of them; the leaves whose distance is set are taken off a set one at a time, whichever comes
// first (TreeFeature.updateLeaves). So which vine hangs where, and which of two leaves that touch
// is the farther one, is decided by the order a HashSet gives its elements in - and that is no
// order at all but what the table happens to be: by bucket, and in a bucket by when an element came
// to it. A tree is vanilla's only if this is the same.
//
// What the order follows from (OpenJDK's HashMap, as it has behaved since Java 8; this is written
// from that behaviour, and held to a JVM's sets by tests/unit/feature_test.cpp):
//
//   - an element's hash is BlockPos.hashCode() - (y + z * 31) * 31 + x - with its upper half folded
//     into the lower; its bucket is the hash's low bits, in a table of 16 buckets that doubles when
//     it holds more than three quarters of its size;
//   - a bucket is a list in the order of arrival, and doubling splits it into two that keep the order;
//   - a bucket that would get a ninth element makes the table double instead while that is smaller
//     than 64, and becomes a red-black tree by hash otherwise. A tree is still walked as a list,
//     but its order is the tree's doing: a new element goes right behind its parent in the tree,
//     and whatever is the root after rebalancing goes to the front. Doubling splits a tree like a
//     list; a half of six or fewer is a list again, a larger one a tree built anew;
//   - taking an element out through an iterator leaves a tree a tree, however small, and does not
//     move its root to the front.
//
// Two positions with one hash in one tree would be ordered by their identity in the JVM - by
// nothing, that is. They do not occur in what a tree spans (it takes 31 blocks along x, or along y
// with one along z); a build with DCHECKs stops there.
#pragma once

#include <vector>

#include "base/types.hpp"

namespace worldgen {

class JavaHashSet {
  public:
    // new HashSet<>(): empty, with no table yet. What it held stays allocated.
    void clear() {
        nodes_.clear();
        free_.clear();
        table_.clear();
        tree_.clear();
        size_ = threshold_ = 0;
        low_ = 0;
    }
    bool empty() const { return size_ == 0; }
    u32 size() const { return size_; }

    // Set.add: false if it is there already.
    bool add(i32 x, i32 y, i32 z);
    bool contains(i32 x, i32 y, i32 z) const;
    // iterator().next() and iterator.remove(): the first element of the order, taken out.
    void pop_first(i32& x, i32& y, i32& z);

    // The elements in the order an iterator gives them.
    template <class F>
    void each(F&& f) const {
        for (const i32 head : table_)
            for (i32 e = head; e >= 0; e = nodes_[static_cast<u32>(e)].next) {
                const Node& n = nodes_[static_cast<u32>(e)];
                f(n.x, n.y, n.z);
            }
    }

    // For tests: how often a bucket became a tree, and how often an element was taken out of one,
    // since the set was made.
    u32 trees_built() const { return trees_built_; }
    u32 taken_from_trees() const { return taken_from_trees_; }

    // Vec3i.hashCode, and HashMap.hash of it.
    static i32 hash_of(i32 x, i32 y, i32 z) {
        const u32 h = (static_cast<u32>(y) + static_cast<u32>(z) * 31u) * 31u + static_cast<u32>(x);
        return static_cast<i32>(h ^ (h >> 16));
    }

  private:
    struct Node {
        i32 x, y, z;
        i32 hash;
        i32 next, prev;           // the list every bucket is, a tree too
        i32 parent, left, right;  // of a tree
        bool red;
    };
    static constexpr u32 kTreeify = 8, kUntreeify = 6, kMinTreeCapacity = 64;

    Node& at(i32 i) { return nodes_[static_cast<u32>(i)]; }
    const Node& at(i32 i) const { return nodes_[static_cast<u32>(i)]; }
    bool is(i32 node, i32 x, i32 y, i32 z, i32 hash) const {
        const Node& n = at(node);
        return n.hash == hash && n.x == x && n.y == y && n.z == z;
    }
    i32 make(i32 x, i32 y, i32 z, i32 hash);
    void grow();
    void split_tree(std::vector<i32>& table, std::vector<u8>& tree, i32 head, u32 index, u32 bit);
    void treeify_bucket(i32 hash);
    void build_tree(std::vector<i32>& table, u32 index);
    bool tree_add(u32 index, i32 x, i32 y, i32 z, i32 hash);
    void tree_remove(u32 index, i32 node);
    i32 root_of(i32 node) const;
    i32 rotate_left(i32 root, i32 p);
    i32 rotate_right(i32 root, i32 p);
    i32 after_insert(i32 root, i32 x);
    i32 after_remove(i32 root, i32 x);
    void root_to_front(std::vector<i32>& table, u32 index, i32 root);

    std::vector<Node> nodes_;
    std::vector<i32> free_;
    std::vector<i32> table_;        // a bucket's first element, -1 for none
    std::vector<u8> tree_;          // whether the bucket is a tree
    std::vector<i32> spare_table_;  // (what grow() fills, so that it allocates nothing once it has grown)
    std::vector<u8> spare_tree_;
    u32 size_ = 0, threshold_ = 0;
    u32 low_ = 0;  // no bucket before this one holds anything
    u32 trees_built_ = 0, taken_from_trees_ = 0;
};

}  // namespace worldgen
