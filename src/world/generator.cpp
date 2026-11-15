#include "world/generator.hpp"

namespace world {

std::vector<FlatGenerator::Layer> FlatGenerator::default_layers() {
    namespace b = proto::gen::block;
    return {{static_cast<BlockState>(b::kBedrock), 1}, {static_cast<BlockState>(b::kDirt), 2}, {static_cast<BlockState>(b::kGrassBlock), 1}};
}

FlatGenerator::FlatGenerator(i32 min_y, u32 sections, std::vector<Layer> layers, u16 biome)
    : min_y_(min_y), sections_(sections), biome_(biome) {
    for (const Layer& l : layers) column_.insert(column_.end(), l.height, l.state);
}

void FlatGenerator::generate(Chunk& c, SectionAllocator& a) {
    for (u32 si = 0; si < c.sections_count; ++si) {
        c.biome[si] = biome_;
        Section& s = c.sections[si];
        const auto at = [&](u32 ly) {
            const usize y = static_cast<usize>(si) * 16 + ly;
            return y < column_.size() ? column_[y] : kAir;
        };
        bool same = true;
        for (u32 ly = 1; ly < 16; ++ly) same &= at(ly) == at(0);
        if (same) {
            s.fill(a, at(0));
            continue;
        }
        BlockState states[Section::kVolume];
        for (u32 ly = 0; ly < 16; ++ly) std::fill_n(states + ly * 256, 256, at(ly));
        s.assign(a, states);
    }
}

}  // namespace world
