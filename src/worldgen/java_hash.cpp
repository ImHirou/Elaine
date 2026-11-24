// SPDX-License-Identifier: GPL-3.0-or-later
#include "worldgen/java_hash.hpp"

#include <utility>

namespace worldgen {

i32 JavaHashSet::make(i32 x, i32 y, i32 z, i32 hash) {
    i32 i;
    if (!free_.empty()) {
        i = free_.back();
        free_.pop_back();
    } else {
        i = static_cast<i32>(nodes_.size());
        nodes_.emplace_back();
    }
    at(i) = Node{x, y, z, hash, -1, -1, -1, -1, -1, false};
    return i;
}

bool JavaHashSet::contains(i32 x, i32 y, i32 z) const {
    if (table_.empty()) return false;
    const i32 hash = hash_of(x, y, z);
    for (i32 e = table_[static_cast<u32>(hash) & (table_.size() - 1)]; e >= 0; e = at(e).next)
        if (is(e, x, y, z, hash)) return true;
    return false;
}

bool JavaHashSet::add(i32 x, i32 y, i32 z) {
    const i32 hash = hash_of(x, y, z);
    if (table_.empty()) grow();
    const u32 index = static_cast<u32>(hash) & static_cast<u32>(table_.size() - 1);
    const i32 first = table_[index];
    if (first < 0) {
        const i32 made = make(x, y, z, hash);
        table_[index] = made;
        tree_[index] = 0;
        if (index < low_) low_ = index;
    } else {
        if (is(first, x, y, z, hash)) return false;
        if (tree_[index]) {
            if (!tree_add(index, x, y, z, hash)) return false;
        } else {
            i32 last = first;
            u32 before = 0;  // how many of the bucket stand before `last`
            for (i32 next; (next = at(last).next) >= 0; last = next, ++before)
                if (is(next, x, y, z, hash)) return false;
            const i32 made = make(x, y, z, hash);
            at(last).next = made;
            // The ninth of a bucket: a tree - or a larger table, while that is a small one.
            if (before >= kTreeify - 1) treeify_bucket(hash);
        }
    }
    if (++size_ > threshold_) grow();
    return true;
}

void JavaHashSet::pop_first(i32& x, i32& y, i32& z) {
    DCHECK(size_ != 0);
    u32 index = low_;
    while (table_[index] < 0) ++index;
    low_ = index;
    const i32 e = table_[index];
    x = at(e).x, y = at(e).y, z = at(e).z;
    if (tree_[index])
        ++taken_from_trees_, tree_remove(index, e);
    else
        table_[index] = at(e).next;
    free_.push_back(e);
    --size_;
}

// The table twice as large: every bucket's elements go to the bucket of the same number or to the
// one that much further on, by the bit of their hash that now counts, in the order they had.
void JavaHashSet::grow() {
    const u32 old_size = static_cast<u32>(table_.size());
    const u32 size = old_size ? old_size * 2 : 16;
    spare_table_.assign(size, -1);
    spare_tree_.assign(size, 0);
    threshold_ = size / 4 * 3;
    low_ = 0;
    for (u32 j = 0; j < old_size; ++j) {
        i32 e = table_[j];
        if (e < 0) continue;
        if (at(e).next < 0) {
            // One element alone stays what it was - a tree of one, if it was a tree.
            const u32 to = static_cast<u32>(at(e).hash) & (size - 1);
            spare_table_[to] = e;
            spare_tree_[to] = tree_[j];
        } else if (tree_[j]) {
            split_tree(spare_table_, spare_tree_, e, j, old_size);
        } else {
            i32 head[2] = {-1, -1}, tail[2] = {-1, -1};
            for (i32 next; e >= 0; e = next) {
                next = at(e).next;
                at(e).next = -1;
                const u32 half = (static_cast<u32>(at(e).hash) & old_size) != 0;
                if (tail[half] < 0)
                    head[half] = e;
                else
                    at(tail[half]).next = e;
                tail[half] = e;
            }
            spare_table_[j] = head[0];
            spare_table_[j + old_size] = head[1];
        }
    }
    table_.swap(spare_table_);
    tree_.swap(spare_tree_);
}

// A tree's bucket split in two as a list is. A half of few elements is a list from then on; a
// larger one is a tree built anew - unless it is the whole of the old tree, which stays as it is.
void JavaHashSet::split_tree(std::vector<i32>& table, std::vector<u8>& tree, i32 head_of, u32 index, u32 bit) {
    i32 head[2] = {-1, -1}, tail[2] = {-1, -1};
    u32 count[2] = {0, 0};
    for (i32 e = head_of, next; e >= 0; e = next) {
        next = at(e).next;
        at(e).next = -1;
        const u32 half = (static_cast<u32>(at(e).hash) & bit) != 0;
        at(e).prev = tail[half];
        if (tail[half] < 0)
            head[half] = e;
        else
            at(tail[half]).next = e;
        tail[half] = e;
        ++count[half];
    }
    for (u32 half = 0; half < 2; ++half) {
        if (head[half] < 0) continue;
        const u32 to = index + (half ? bit : 0);
        table[to] = head[half];
        if (count[half] <= kUntreeify) {
            tree[to] = 0;
        } else {
            tree[to] = 1;
            if (head[half ^ 1] >= 0) build_tree(table, to);
        }
    }
}

void JavaHashSet::treeify_bucket(i32 hash) {
    if (table_.size() < kMinTreeCapacity) {
        grow();
        return;
    }
    const u32 index = static_cast<u32>(hash) & static_cast<u32>(table_.size() - 1);
    i32 before = -1;
    for (i32 e = table_[index]; e >= 0; e = at(e).next) {
        at(e).prev = before;
        before = e;
    }
    tree_[index] = 1;
    build_tree(table_, index);
}

// The bucket's list made a tree: its elements put in one after the other, by hash.
void JavaHashSet::build_tree(std::vector<i32>& table, u32 index) {
    ++trees_built_;
    i32 root = -1;
    for (i32 x = table[index], next; x >= 0; x = next) {
        next = at(x).next;
        at(x).left = at(x).right = -1;
        if (root < 0) {
            at(x).parent = -1;
            at(x).red = false;
            root = x;
            continue;
        }
        const i32 hash = at(x).hash;
        for (i32 p = root;;) {
            DCHECK(at(p).hash != hash);  // (the JVM would go by identity)
            const bool left = at(p).hash >= hash;
            const i32 child = left ? at(p).left : at(p).right;
            if (child < 0) {
                at(x).parent = p;
                (left ? at(p).left : at(p).right) = x;
                root = after_insert(root, x);
                break;
            }
            p = child;
        }
    }
    root_to_front(table, index, root);
}

i32 JavaHashSet::root_of(i32 node) const {
    while (at(node).parent >= 0) node = at(node).parent;
    return node;
}

// A new element of a tree: a leaf where its hash puts it, and in the list right behind its parent.
bool JavaHashSet::tree_add(u32 index, i32 x, i32 y, i32 z, i32 hash) {
    const i32 first = table_[index];
    const i32 root = root_of(first);
    for (i32 p = root;;) {
        const i32 at_p = at(p).hash;
        bool left;
        if (at_p > hash) {
            left = true;
        } else if (at_p < hash) {
            left = false;
        } else if (is(p, x, y, z, hash)) {
            return false;
        } else {
            for (i32 e = first; e >= 0; e = at(e).next)
                if (is(e, x, y, z, hash)) return false;
            DCHECK(false);  // (one hash, two positions: the JVM would go by identity)
            left = true;
        }
        const i32 child = left ? at(p).left : at(p).right;
        if (child < 0) {
            const i32 made = make(x, y, z, hash);
            const i32 behind = at(p).next;
            at(made).next = behind;
            (left ? at(p).left : at(p).right) = made;
            at(p).next = made;
            at(made).parent = at(made).prev = p;
            if (behind >= 0) at(behind).prev = made;
            root_to_front(table_, index, after_insert(root, made));
            return true;
        }
        p = child;
    }
}

void JavaHashSet::root_to_front(std::vector<i32>& table, u32 index, i32 root) {
    const i32 first = table[index];
    if (root < 0 || root == first) return;
    table[index] = root;
    const i32 before = at(root).prev, behind = at(root).next;
    if (behind >= 0) at(behind).prev = before;
    if (before >= 0) at(before).next = behind;
    if (first >= 0) at(first).prev = root;
    at(root).next = first;
    at(root).prev = -1;
}

// p's right child takes p's place, p becomes its left child. A node that becomes the root this
// way is black.
i32 JavaHashSet::rotate_left(i32 root, i32 p) {
    if (p < 0) return root;
    const i32 r = at(p).right;
    if (r < 0) return root;
    const i32 inner = at(r).left;
    at(p).right = inner;
    if (inner >= 0) at(inner).parent = p;
    const i32 above = at(p).parent;
    at(r).parent = above;
    if (above < 0) {
        root = r;
        at(r).red = false;
    } else if (at(above).left == p) {
        at(above).left = r;
    } else {
        at(above).right = r;
    }
    at(r).left = p;
    at(p).parent = r;
    return root;
}

i32 JavaHashSet::rotate_right(i32 root, i32 p) {
    if (p < 0) return root;
    const i32 l = at(p).left;
    if (l < 0) return root;
    const i32 inner = at(l).right;
    at(p).left = inner;
    if (inner >= 0) at(inner).parent = p;
    const i32 above = at(p).parent;
    at(l).parent = above;
    if (above < 0) {
        root = l;
        at(l).red = false;
    } else if (at(above).right == p) {
        at(above).right = l;
    } else {
        at(above).left = l;
    }
    at(l).right = p;
    at(p).parent = l;
    return root;
}

// A red-black tree put right after a red leaf x was hung into it: the new root.
i32 JavaHashSet::after_insert(i32 root, i32 x) {
    at(x).red = true;
    for (;;) {
        i32 parent = at(x).parent;
        if (parent < 0) {
            at(x).red = false;
            return x;
        }
        if (!at(parent).red) return root;
        i32 grand = at(parent).parent;
        if (grand < 0) return root;
        const bool on_left = at(grand).left == parent;
        const i32 uncle = on_left ? at(grand).right : at(grand).left;
        if (uncle >= 0 && at(uncle).red) {
            at(uncle).red = false;
            at(parent).red = false;
            at(grand).red = true;
            x = grand;
            continue;
        }
        // The uncle is black: x is turned to the outside of its parent if it is on the inside, then
        // the parent is lifted over the grandparent.
        if (x == (on_left ? at(parent).right : at(parent).left)) {
            x = parent;
            root = on_left ? rotate_left(root, x) : rotate_right(root, x);
            parent = at(x).parent;
            grand = parent < 0 ? -1 : at(parent).parent;
        }
        if (parent >= 0) {
            at(parent).red = false;
            if (grand >= 0) {
                at(grand).red = true;
                root = on_left ? rotate_right(root, grand) : rotate_left(root, grand);
            }
        }
    }
}

// ...and after a black node went: x stands where it stood, one black short. The new root.
i32 JavaHashSet::after_remove(i32 root, i32 x) {
    for (;;) {
        if (x < 0 || x == root) return root;
        i32 parent = at(x).parent;
        if (parent < 0) {
            at(x).red = false;
            return x;
        }
        if (at(x).red) {
            at(x).red = false;
            return root;
        }
        const bool on_left = at(parent).left == x;
        const auto near_of = [&](i32 n) { return on_left ? at(n).left : at(n).right; };
        const auto far_of = [&](i32 n) { return on_left ? at(n).right : at(n).left; };
        const auto lift = [&](i32 n) { return on_left ? rotate_left(root, n) : rotate_right(root, n); };  // n's far child over n
        const auto lower = [&](i32 n) {
            return on_left ? rotate_right(root, n) : rotate_left(root, n);
        };  // n's near child over n
        i32 sibling = far_of(parent);
        if (sibling >= 0 && at(sibling).red) {
            at(sibling).red = false;
            at(parent).red = true;
            root = lift(parent);
            parent = at(x).parent;
            sibling = parent < 0 ? -1 : far_of(parent);
        }
        if (sibling < 0) {
            x = parent;
            continue;
        }
        const i32 near = near_of(sibling);
        i32 far = far_of(sibling);
        const bool far_black = far < 0 || !at(far).red;
        if (far_black && (near < 0 || !at(near).red)) {
            at(sibling).red = true;
            x = parent;
            continue;
        }
        if (far_black) {
            if (near >= 0) at(near).red = false;
            at(sibling).red = true;
            root = lower(sibling);
            parent = at(x).parent;
            sibling = parent < 0 ? -1 : far_of(parent);
        }
        if (sibling >= 0) {
            at(sibling).red = parent < 0 ? false : at(parent).red;
            far = far_of(sibling);
            if (far >= 0) at(far).red = false;
        }
        if (parent >= 0) {
            at(parent).red = false;
            root = lift(parent);
        }
        x = root;
    }
}

// An element taken out of a tree through an iterator: out of the list, and out of the tree as a
// node is taken out of any red-black tree - where it has two children, the next one by hash takes
// its place and its colour. The tree stays a tree, and its root stays where it is in the list.
void JavaHashSet::tree_remove(u32 index, i32 p) {
    i32 first = table_[index];
    i32 root = first;
    const i32 behind = at(p).next, before = at(p).prev;
    if (before < 0)
        table_[index] = first = behind;
    else
        at(before).next = behind;
    if (behind >= 0) at(behind).prev = before;
    if (first < 0) return;
    root = root_of(root);

    const i32 left = at(p).left, right = at(p).right;
    i32 replacement;
    if (left >= 0 && right >= 0) {
        i32 s = right;
        while (at(s).left >= 0) s = at(s).left;
        std::swap(at(s).red, at(p).red);
        const i32 s_right = at(s).right;
        const i32 above = at(p).parent;
        if (s == right) {
            at(p).parent = s;
            at(s).right = p;
        } else {
            const i32 s_parent = at(s).parent;
            at(p).parent = s_parent;
            if (s_parent >= 0) {
                if (at(s_parent).left == s)
                    at(s_parent).left = p;
                else
                    at(s_parent).right = p;
            }
            at(s).right = right;
            at(right).parent = s;
        }
        at(p).left = -1;
        at(p).right = s_right;
        if (s_right >= 0) at(s_right).parent = p;
        at(s).left = left;
        at(left).parent = s;
        at(s).parent = above;
        if (above < 0)
            root = s;
        else if (at(above).left == p)
            at(above).left = s;
        else
            at(above).right = s;
        replacement = s_right >= 0 ? s_right : p;
    } else if (left >= 0) {
        replacement = left;
    } else if (right >= 0) {
        replacement = right;
    } else {
        replacement = p;
    }
    if (replacement != p) {
        const i32 above = at(p).parent;
        at(replacement).parent = above;
        if (above < 0) {
            root = replacement;
            at(replacement).red = false;
        } else if (at(above).left == p) {
            at(above).left = replacement;
        } else {
            at(above).right = replacement;
        }
        at(p).left = at(p).right = at(p).parent = -1;
    }
    if (!at(p).red) after_remove(root, replacement);
    if (replacement == p) {
        const i32 above = at(p).parent;
        at(p).parent = -1;
        if (above >= 0) {
            if (at(above).left == p)
                at(above).left = -1;
            else if (at(above).right == p)
                at(above).right = -1;
        }
    }
}

}  // namespace worldgen
