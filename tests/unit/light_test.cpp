// Light engine tests.
//
// The engine is a port of vanilla's incremental algorithm. What that algorithm is supposed to
// reach is simple to state: sky light is 15 wherever the column is open to the sky, an emitting
// block has at least its emission, and every other cell has the best of its six neighbours minus
// max(1, its own dampening), unless the two touching faces close the gap. `Reference` computes that
// fixed point the slow, obvious way; the engine - after first lighting, and after any sequence of
// block edits - has to agree with it everywhere light is stored. (Where vanilla's sparse storage
// makes light differ from the fixed point is covered by the tests that do not use Reference.)
#include <gtest/gtest.h>

#include <functional>
#include <random>
#include <vector>

#include "proto/frame.hpp"
#include "proto/wire.hpp"
#include "world/chunk_codec.hpp"
#include "world/dimension.hpp"

using namespace world;
namespace blk = proto::gen::block;

namespace {

constexpr BlockState S(unsigned id) { return static_cast<BlockState>(id); }

class Reference {
  public:
    Reference(const Dimension& d, i32 x0, i32 x1, i32 y0, i32 y1, i32 z0, i32 z1)
        : d_(d), x0_(x0), y0_(y0), z0_(z0), nx_(x1 - x0), ny_(y1 - y0), nz_(z1 - z0) {
        const usize n = static_cast<usize>(nx_) * ny_ * nz_;
        state_.resize(n);
        sky_.assign(n, 0);
        block_.assign(n, 0);
        lowest_.assign(static_cast<usize>(nx_) * nz_, INT32_MIN);
        for (i32 x = 0; x < nx_; ++x)
            for (i32 z = 0; z < nz_; ++z) {
                for (i32 y = 0; y < ny_; ++y) state_[at(x, y, z)] = d.block(x0 + x, y0 + y, z0 + z);
                // ChunkSkyLightSources: walking down from above the world, full sky light ends at
                // the first block that dampens it or whose top face closes against the one above.
                BlockState top = kAir;
                for (i32 y = d.max_y(); y >= d.min_y(); --y) {
                    const BlockState bottom = d.block(x0 + x, y, z0 + z);
                    if (dampening(bottom) != 0 || shape_occludes(top, bottom, kDown)) {
                        lowest_[static_cast<usize>(x) * nz_ + z] = y + 1;
                        break;
                    }
                    top = bottom;
                }
            }
    }

    // `outside_sky(y)`: sky light of cells beyond the box (the caller keeps edits far from its walls).
    void compute(const std::function<u32(i32)>& outside_sky) {
        for (i32 x = 0; x < nx_; ++x)
            for (i32 z = 0; z < nz_; ++z)
                for (i32 y = 0; y < ny_; ++y) {
                    const usize i = at(x, y, z);
                    sky_[i] = (y0_ + y >= lowest_[static_cast<usize>(x) * nz_ + z]) ? 15 : 0;
                    block_[i] = static_cast<u8>(emission(state_[i]));
                }
        relax(sky_, true, outside_sky);
        relax(block_, false, [](i32) { return 0u; });
    }

    u32 sky(i32 x, i32 y, i32 z) const { return sky_[at(x - x0_, y - y0_, z - z0_)]; }
    u32 block(i32 x, i32 y, i32 z) const { return block_[at(x - x0_, y - y0_, z - z0_)]; }
    i32 lowest_source(i32 x, i32 z) const { return lowest_[static_cast<usize>(x - x0_) * nz_ + (z - z0_)]; }

  private:
    usize at(i32 x, i32 y, i32 z) const { return (static_cast<usize>(x) * nz_ + z) * ny_ + y; }

    void relax(std::vector<u8>& light, bool is_sky, const std::function<u32(i32)>& outside) {
        for (bool changed = true; changed;) {
            changed = false;
            for (int pass = 0; pass < 2; ++pass) {  // forwards then backwards: converges in a few sweeps
                for (i32 xi = 0; xi < nx_; ++xi)
                    for (i32 zi = 0; zi < nz_; ++zi)
                        for (i32 yi = 0; yi < ny_; ++yi) {
                            const i32 x = pass ? nx_ - 1 - xi : xi, z = pass ? nz_ - 1 - zi : zi, y = pass ? ny_ - 1 - yi : yi;
                            const usize i = at(x, y, z);
                            if (is_sky && y0_ + y >= lowest_[static_cast<usize>(x) * nz_ + z]) continue;  // a source stays 15
                            const BlockState to = state_[i];
                            u32 best = light[i];
                            for (u32 dir = 0; dir < 6; ++dir) {  // dir = the way the light travels, from neighbour to here
                                const i32 fx = x - kStepX[dir], fy = y - kStepY[dir], fz = z - kStepZ[dir];
                                const bool inside = fx >= 0 && fx < nx_ && fy >= 0 && fy < ny_ && fz >= 0 && fz < nz_;
                                const u32 from_level = inside ? light[at(fx, fy, fz)] : outside(y0_ + fy);
                                const BlockState from = inside ? state_[at(fx, fy, fz)] : d_.block(x0_ + fx, y0_ + fy, z0_ + fz);
                                if (from_level <= opacity(to) || shape_occludes(from, to, dir)) continue;
                                best = std::max(best, from_level - opacity(to));
                            }
                            if (best != light[i]) {
                                light[i] = static_cast<u8>(best);
                                changed = true;
                            }
                        }
            }
        }
    }

    const Dimension& d_;
    i32 x0_, y0_, z0_, nx_, ny_, nz_;
    std::vector<BlockState> state_;
    std::vector<u8> sky_, block_;
    std::vector<i32> lowest_;
};

struct FlatWorld {
    FlatGenerator gen{-64, 24, FlatGenerator::default_layers(), 39};
    Dimension dim{proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression};
    explicit FlatWorld(i32 radius = 0) {
        for (i32 x = -radius; x <= radius; ++x)
            for (i32 z = -radius; z <= radius; ++z) dim.frame(x, z);
    }
    u32 sky(i32 x, i32 y, i32 z) const { return dim.light().light(LightEngine::kSky, x, y, z); }
    u32 block(i32 x, i32 y, i32 z) const { return dim.light().light(LightEngine::kBlock, x, y, z); }
    void settle() {
        dim.update_light();
        dim.end_tick();
    }
};

u32 flat_sky(i32 y) { return y >= -60 ? 15 : 0; }

// Compare the engine with the fixed point in a box of blocks around the origin chunk.
void expect_fixed_point(FlatWorld& w, i32 lo, i32 hi, i32 y0, i32 y1, const char* what) {
    Reference ref(w.dim, lo, hi, y0, y1, lo, hi);
    ref.compute(flat_sky);
    u32 bad = 0;
    for (i32 x = lo; x < hi; ++x)
        for (i32 z = lo; z < hi; ++z) {
            const Chunk& c = *w.dim.find(x >> 4, z >> 4);
            const i32 lowest = w.dim.light().lowest_source_y(c, static_cast<u32>(x & 15), static_cast<u32>(z & 15));
            if (lowest != ref.lowest_source(x, z) && ++bad < 6)
                ADD_FAILURE() << what << ": lowest source at " << x << "," << z << " is " << lowest << ", expected "
                              << ref.lowest_source(x, z);
            for (i32 y = y0; y < y1; ++y) {
                if (w.sky(x, y, z) != ref.sky(x, y, z) && ++bad < 6)
                    ADD_FAILURE() << what << ": sky light at " << x << "," << y << "," << z << " is " << w.sky(x, y, z)
                                  << ", expected " << ref.sky(x, y, z);
                if (w.block(x, y, z) != ref.block(x, y, z) && ++bad < 6)
                    ADD_FAILURE() << what << ": block light at " << x << "," << y << "," << z << " is " << w.block(x, y, z)
                                  << ", expected " << ref.block(x, y, z);
            }
        }
    EXPECT_EQ(bad, 0u) << what;
}

}  // namespace

TEST(Light, FlatWorldIsLitFromTheSurfaceUp) {
    FlatWorld w;
    for (i32 x : {0, 7, 15})
        for (i32 z : {0, 9, 15}) {
            EXPECT_EQ(w.sky(x, -60, z), 15u);
            EXPECT_EQ(w.sky(x, -49, z), 15u);
            EXPECT_EQ(w.sky(x, -33, z), 15u);
            EXPECT_EQ(w.sky(x, 100, z), 15u);  // no storage up there: implied
            EXPECT_EQ(w.sky(x, 319, z), 15u);
            EXPECT_EQ(w.sky(x, -61, z), 0u);  // inside the grass block
            EXPECT_EQ(w.sky(x, -64, z), 0u);
            EXPECT_EQ(w.sky(x, -70, z), 0u);  // the stored section below the world
            EXPECT_EQ(w.block(x, -60, z), 0u);
            EXPECT_EQ(w.dim.light().lowest_source_y(*w.dim.find(0, 0), static_cast<u32>(x), static_cast<u32>(z)), -60);
        }
    const Chunk& c = *w.dim.find(0, 0);
    // Light is stored only next to blocks: the section with the ground and the ones touching it.
    for (u32 li = 0; li < 26; ++li) {
        EXPECT_EQ(c.sky[li].present(), li <= 2) << li;
        EXPECT_EQ(c.block[li].present(), li <= 2) << li;
    }
    EXPECT_TRUE(c.sky[0].vanilla_empty());
    EXPECT_FALSE(c.sky[2].vanilla_empty());
    EXPECT_EQ(c.sky_top, -2);  // highest stored section (-3) + 1
    expect_fixed_point(w, -8, 24, -80, -16, "untouched flat world");
}

TEST(Light, GlowstoneLightsUpAndGoesDarkAgain) {
    FlatWorld w(1);
    ASSERT_TRUE(w.dim.set_block(8, -55, 8, S(blk::kGlowstone)));
    w.settle();
    EXPECT_EQ(w.block(8, -55, 8), 15u);
    EXPECT_EQ(w.block(8, -54, 8), 14u);
    EXPECT_EQ(w.block(9, -54, 8), 13u);
    EXPECT_EQ(w.block(8, -60, 8), 10u);
    EXPECT_EQ(w.block(8, -61, 8), 0u);   // the ground does not take light
    EXPECT_EQ(w.block(22, -55, 8), 1u);  // 14 blocks away, in the next chunk
    EXPECT_EQ(w.block(23, -55, 8), 0u);
    EXPECT_EQ(w.sky(8, -56, 8), 14u);  // and it shades the column below itself
    expect_fixed_point(w, -16, 32, -80, -16, "glowstone placed");

    ASSERT_TRUE(w.dim.set_block(8, -55, 8, kAir));
    w.settle();
    expect_fixed_point(w, -16, 32, -80, -16, "glowstone removed");
    EXPECT_EQ(w.block(8, -55, 8), 0u);
    EXPECT_EQ(w.sky(8, -56, 8), 15u);
    // The layer was written and is dark again: vanilla keeps the array, so it is no longer "empty".
    const Chunk& c = *w.dim.find(0, 0);
    EXPECT_TRUE(c.block[1].present());
    EXPECT_FALSE(c.block[1].vanilla_empty());
    EXPECT_TRUE(c.block[0].vanilla_empty());  // never touched
}

TEST(Light, RoofShadesWhatIsUnderIt) {
    FlatWorld w(1);
    for (i32 x = 6; x <= 10; ++x)
        for (i32 z = 6; z <= 10; ++z) ASSERT_TRUE(w.dim.set_block(x, -50, z, S(blk::kStone)));
    w.settle();
    EXPECT_EQ(w.sky(8, -49, 8), 15u);  // on top
    EXPECT_EQ(w.sky(8, -50, 8), 0u);   // inside
    EXPECT_EQ(w.sky(8, -51, 8), 12u);  // centre: three steps from the open sky
    EXPECT_EQ(w.sky(6, -51, 6), 14u);  // corner: one step
    EXPECT_EQ(w.sky(8, -60, 8), 12u);  // the same all the way down
    EXPECT_EQ(w.sky(5, -51, 8), 15u);  // beside the roof
    expect_fixed_point(w, -16, 32, -80, -16, "stone roof");
    // Glass lets the sky through.
    for (i32 x = 6; x <= 10; ++x)
        for (i32 z = 6; z <= 10; ++z) ASSERT_TRUE(w.dim.set_block(x, -50, z, S(blk::kGlass)));
    w.settle();
    EXPECT_EQ(w.sky(8, -51, 8), 15u);
    EXPECT_EQ(w.sky(8, -50, 8), 15u);
    expect_fixed_point(w, -16, 32, -80, -16, "glass roof");
    // Leaves take one level and stop the direct sky light.
    for (i32 x = 6; x <= 10; ++x)
        for (i32 z = 6; z <= 10; ++z) ASSERT_TRUE(w.dim.set_block(x, -50, z, S(blk::kOakLeaves)));
    w.settle();
    EXPECT_EQ(w.sky(8, -50, 8), 14u);
    EXPECT_EQ(w.sky(8, -51, 8), 13u);
    expect_fixed_point(w, -16, 32, -80, -16, "leaf roof");
}

TEST(Light, BlockInTheSkyCreatesAndReleasesStorage) {
    FlatWorld w(1);
    const Chunk& c = *w.dim.find(0, 0);
    const Chunk& east = *w.dim.find(1, 0);
    ASSERT_TRUE(w.dim.set_block(8, 100, 8, S(blk::kStone)));  // section y = 6, light section 11
    w.settle();
    // Its section and the 26 around it now store light, in this chunk and in the neighbours.
    for (u32 li = 3; li < 26; ++li) {
        EXPECT_EQ(c.sky[li].present(), li >= 10 && li <= 12) << li;
        EXPECT_EQ(east.block[li].present(), li >= 10 && li <= 12) << li;
    }
    EXPECT_EQ(c.sky_top, 8);
    EXPECT_EQ(w.sky(8, 101, 8), 15u);
    EXPECT_EQ(w.sky(8, 100, 8), 0u);
    EXPECT_EQ(w.sky(8, 99, 8), 14u);   // stored
    EXPECT_EQ(w.sky(8, 81, 8), 14u);   // stored (section 5)
    EXPECT_EQ(w.sky(8, 50, 8), 14u);   // not stored: the bottom layer of the first stored section above
    EXPECT_EQ(w.sky(8, -40, 8), 14u);  // stored again near the ground
    EXPECT_EQ(w.sky(8, -60, 8), 14u);
    EXPECT_EQ(w.sky(9, 50, 8), 15u);
    EXPECT_EQ(w.dim.light().lowest_source_y(c, 8, 8), 101);

    ASSERT_TRUE(w.dim.set_block(8, 100, 8, kAir));
    w.settle();
    for (u32 li = 3; li < 26; ++li) {
        EXPECT_FALSE(c.sky[li].present()) << li;
        EXPECT_FALSE(c.block[li].present()) << li;
        EXPECT_FALSE(east.sky[li].present()) << li;
    }
    EXPECT_EQ(c.sky_top, -2);
    EXPECT_EQ(w.sky(8, 99, 8), 15u);
    EXPECT_EQ(w.sky(8, 50, 8), 15u);
    EXPECT_EQ(w.sky(8, -60, 8), 15u);
    EXPECT_EQ(w.dim.light().lowest_source_y(c, 8, 8), -60);
    expect_fixed_point(w, -16, 32, -80, -16, "after the block in the sky is gone");
    // All light arrays went back to the pool except the ones the flat world itself needs.
    w.dim.frame(0, 0);
}

TEST(Light, ChangesAreRecordedForTheSectionsTheyTouch) {
    FlatWorld w(1);
    Chunk& c = *w.dim.find(0, 0);
    Chunk& east = *w.dim.find(1, 0);
    ASSERT_TRUE(w.dim.set_block(8, -55, 8, S(blk::kTorch)));  // light 14: reaches into section y = -3 above
    w.dim.update_light();
    EXPECT_EQ(c.block_changed, 0b110u);
    EXPECT_EQ(c.sky_changed, 0u);  // a torch does not dampen sky light
    EXPECT_TRUE(c.light_listed);
    EXPECT_FALSE(c.frame_valid);
    // 14 - 8 = light 6 at the east border: crosses into the next chunk. The lit cell at (15, -49, 8)
    // touches the section above AND the chunk to the east, so (as SectionPos.aroundAndAtBlockPos has
    // it) the eastern chunk's upper section counts as affected too. To the west the light at that
    // height stops one block short of the border.
    EXPECT_EQ(east.block_changed, 0b110u);
    EXPECT_EQ(w.dim.find(-1, 0)->block_changed, 0b010u);
    EXPECT_EQ(w.dim.light_changes().size(), 5u);  // this chunk and its four side neighbours; the corners stay dark
    w.dim.end_tick();
    EXPECT_EQ(c.block_changed, 0u);
    EXPECT_FALSE(c.light_listed);
    EXPECT_TRUE(w.dim.light_changes().empty());

    // Stone next to it changes sky light only where it stands, and block light where it shadows.
    ASSERT_TRUE(w.dim.set_block(8, -55, 9, S(blk::kStone)));
    w.dim.update_light();
    EXPECT_EQ(c.sky_changed, 0b010u);
    EXPECT_NE(c.block_changed, 0u);
    w.dim.end_tick();
    expect_fixed_point(w, -16, 32, -80, -16, "torch and stone");
}

TEST(Light, RandomEditsStayOnTheFixedPoint) {
    // Opaque, clear, dampening, shaped (slabs, stairs, paths, snow layers) and emitting states.
    std::vector<BlockState> pool = {kAir,
                                    kAir,
                                    kAir,
                                    S(blk::kStone),
                                    S(blk::kStone),
                                    S(blk::kGlass),
                                    S(blk::kIce),
                                    S(blk::kOakLeaves),
                                    S(blk::kWater),
                                    S(blk::kCobweb),
                                    S(blk::kTintedGlass),
                                    S(blk::kGlowstone),
                                    S(blk::kSeaLantern),
                                    S(blk::kTorch),
                                    S(blk::kLantern),
                                    S(blk::kMagmaBlock),
                                    S(blk::kEndRod),
                                    S(blk::kFarmland),
                                    S(blk::kDirtPath),
                                    S(blk::kOakFence),
                                    S(blk::kIronBars)};
    // Every state of a slab, a stair and the snow layer block: all the face shapes there are.
    for (unsigned b : {blk::kOakSlab, blk::kOakStairs, blk::kSnow}) {
        const BlockInfo& info = block_info_of(S(b));
        for (u32 i = 0; i < info.state_count; ++i) pool.push_back(S(info.first_state + i));
    }
    for (u32 seed : {1u, 2u, 3u}) {
        FlatWorld w(2);
        std::mt19937 rng(seed);
        std::vector<std::array<i32, 3>> placed;
        for (int round = 0; round < 12; ++round) {
            const int edits = round < 4 ? 120 : 25;
            for (int n = 0; n < edits; ++n) {
                if (!placed.empty() && rng() % 3 == 0) {  // remove or replace something that is there
                    const auto p = placed[rng() % placed.size()];
                    w.dim.set_block(p[0], p[1], p[2], rng() % 2 ? kAir : pool[rng() % pool.size()]);
                    continue;
                }
                // Dense around the chunk corner at (16, 16) so that light keeps crossing borders.
                const i32 x = 8 + static_cast<i32>(rng() % 16), z = 8 + static_cast<i32>(rng() % 16);
                const i32 y = -61 + static_cast<i32>(rng() % 14);  // digs into the grass layer too
                w.dim.set_block(x, y, z, pool[rng() % pool.size()]);
                placed.push_back({x, y, z});
            }
            if (round % 3 == 2) w.dim.frame(1, 1);  // re-encoding folds uniform arrays: must not change a value
            w.settle();
            expect_fixed_point(w, -16, 48, -80, -16,
                               ("seed " + std::to_string(seed) + " round " + std::to_string(round)).c_str());
            if (::testing::Test::HasFailure()) return;
        }
        // Take everything out again: the world must be exactly as lit as an untouched one.
        for (const auto& p : placed) w.dim.set_block(p[0], p[1], p[2], p[1] == -61 ? S(blk::kGrassBlock) : kAir);
        w.settle();
        expect_fixed_point(w, -16, 48, -80, -16, "everything removed");
        FlatWorld fresh(2);
        for (i32 x = -16; x < 48; x += 3)
            for (i32 z = -16; z < 48; z += 3)
                for (i32 y = -80; y < 0; ++y) {
                    ASSERT_EQ(w.sky(x, y, z), fresh.sky(x, y, z));
                    ASSERT_EQ(w.block(x, y, z), fresh.block(x, y, z));
                }
    }
}

namespace {

// A world whose chunks around the origin come out of the generator with light sources, overhangs
// and shaped blocks right at the chunk borders: first lighting has to carry light across them.
struct PatternGenerator final : ChunkGenerator {
    FlatGenerator flat{-64, 24, FlatGenerator::default_layers(), 39};
    void generate(Chunk& c, SectionAllocator& a) override {
        flat.generate(c, a);
        if (std::abs(c.x) > 1 || std::abs(c.z) > 1) return;
        const auto put = [&](u32 x, i32 y, u32 z, unsigned s) {
            c.sections[static_cast<u32>(y + 64) >> 4].set(a, Section::index(x, static_cast<u32>(y & 15), z), S(s));
        };
        std::mt19937 rng(static_cast<u32>(c.x * 31 + c.z * 17 + 100));
        for (u32 i = 0; i < 16; ++i) {
            put(i, -52, 0, blk::kStone);  // a roof along the north edge, continued by the neighbour's south edge
            put(i, -52, 15, blk::kStone);
            put(0, -50, i, blk::kOakLeaves);
            put(15, -50, i, blk::kOakSlab);
        }
        put(15, -58, 15, blk::kGlowstone);  // in the corner: lights four chunks
        put(0, -56, 7, blk::kSeaLantern);
        put(8, -40, 8, blk::kStone);  // reaches into the next section up
        for (int n = 0; n < 40; ++n)
            put(rng() % 16, -60 + static_cast<i32>(rng() % 6), rng() % 16, rng() % 4 ? blk::kStone : blk::kTorch);
    }
    i32 sea_level() const override { return -63; }
    i32 spawn_height() const override { return -60; }
    bool is_flat() const override { return true; }
};

}  // namespace

TEST(Light, FirstLightingCrossesChunkBordersInAnyOrder) {
    // The same world lit in two different chunk orders must end up identical, and on the fixed point.
    std::vector<std::pair<i32, i32>> order;
    for (i32 x = -2; x <= 2; ++x)
        for (i32 z = -2; z <= 2; ++z) order.push_back({x, z});
    std::vector<std::vector<u8>> results;
    for (int variant = 0; variant < 3; ++variant) {
        PatternGenerator gen;
        Dimension dim(proto::reg::dimension_type::kOverworld, "minecraft:overworld", gen, proto::kNoCompression);
        if (variant == 1) std::reverse(order.begin(), order.end());
        if (variant == 2) std::shuffle(order.begin(), order.end(), std::mt19937(5));
        for (const auto& [x, z] : order) dim.frame(x, z);

        Reference ref(dim, -32, 48, -80, -16, -32, 48);
        ref.compute(flat_sky);
        u32 bad = 0;
        std::vector<u8> values;
        for (i32 x = -32; x < 48; ++x)
            for (i32 z = -32; z < 48; ++z)
                for (i32 y = -80; y < -16; ++y) {
                    const u32 sky = dim.light().light(LightEngine::kSky, x, y, z),
                              block = dim.light().light(LightEngine::kBlock, x, y, z);
                    values.push_back(static_cast<u8>(sky << 4 | block));
                    if ((sky != ref.sky(x, y, z) || block != ref.block(x, y, z)) && ++bad < 6)
                        ADD_FAILURE() << "variant " << variant << " at " << x << "," << y << "," << z << ": sky " << sky << "/"
                                      << ref.sky(x, y, z) << " block " << block << "/" << ref.block(x, y, z);
                }
        EXPECT_EQ(bad, 0u);
        results.push_back(std::move(values));
    }
    EXPECT_EQ(results[0], results[1]);
    EXPECT_EQ(results[0], results[2]);
}

TEST(Light, WireMasksFollowVanillaLayerStates) {
    FlatWorld w(1);
    const auto masks = [&](const Chunk& c) {
        std::vector<u8> buf(light_wire_bound(c, ~0u, ~0u) + 64);
        const u8* end = put_light(buf.data(), c, ~0u, ~0u);
        proto::Reader r(buf.data(), static_cast<usize>(end - buf.data()));
        std::array<u64, 4> m{};
        for (u64& x : m) {  // ByteBufCodecs.BIT_SET: VarInt byte count, little-endian bytes
            const auto bytes = r.byte_array(8);
            EXPECT_TRUE(bytes.empty() || bytes.back() != 0) << "bit set with a trailing zero byte";
            x = 0;
            for (usize i = 0; i < bytes.size(); ++i) x |= u64{bytes[i]} << (8 * i);
        }
        return m;  // sky, block, empty sky, empty block
    };
    const Chunk& c = *w.dim.find(0, 0);
    EXPECT_EQ(masks(c), (std::array<u64, 4>{0b110, 0, 0b001, 0b111}));
    // A light source: its section and the one above get block light arrays.
    w.dim.set_block(8, -50, 8, S(blk::kGlowstone));
    w.settle();
    EXPECT_EQ(masks(c), (std::array<u64, 4>{0b110, 0b110, 0b001, 0b001}));
    // Gone again: the arrays stay (all zero) - written layers are not "empty" in vanilla.
    w.dim.set_block(8, -50, 8, kAir);
    w.settle();
    EXPECT_EQ(masks(c), (std::array<u64, 4>{0b110, 0b110, 0b001, 0b001}));
    // Re-encoding folds the all-zero arrays away but must keep reporting them as written.
    w.dim.frame(0, 0);
    EXPECT_FALSE(c.block[1].has_array());
    EXPECT_EQ(masks(c), (std::array<u64, 4>{0b110, 0b110, 0b001, 0b001}));
    EXPECT_EQ(w.block(8, -50, 8), 0u);
}
