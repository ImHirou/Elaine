// Micro-benchmarks: the network hot paths here, the world in world_bench.cpp, entities in
// entity_bench.cpp. Usage: bench [filter-substring]
// Numbers are only meaningful from an optimized build (preset `release` or `native`), on a quiet
// machine; compare before/after on the same machine and report both.
#include <random>
#include <string>
#include <vector>

#include "bench.hpp"

#include "base/buf.hpp"
#include "crypto/cfb8.hpp"
#include "proto/frame.hpp"
#include "proto/packets.hpp"
#include "proto/varint.hpp"

using namespace benchmark;

int main(int argc, char** argv) {
    if (argc > 1) g_filter = argv[1];
    std::mt19937 rng(1);

    // ---------------------------------------------------------------- varint
    {
        constexpr usize kN = 1 << 16;
        std::vector<u32> small(kN), mixed(kN);
        for (auto& v : small) v = rng() % 128;            // packet ids, small lengths
        for (auto& v : mixed) v = rng() >> (rng() % 32);  // block states, entity ids, ...
        std::vector<u8> buf(kN * 5 + 64);
        for (auto* set : {&small, &mixed}) {
            const char* tag = set == &small ? "1-byte" : "mixed";
            std::string name = std::string("varint encode (") + tag + ")";
            bench(name.c_str(), kN, "M values/s", [&] {
                u8* p = buf.data();
                for (u32 v : *set) p = proto::put_varint(p, v);
                g_sink = static_cast<u64>(p - buf.data());
            });
            u8* end = buf.data();
            for (u32 v : *set) end = proto::put_varint(end, v);
            std::memset(end, 0, 16);
            name = std::string("varint decode (") + tag + ")";
            bench(name.c_str(), kN, "M values/s", [&] {
                const u8* p = buf.data();
                u64 sum = 0;
                for (usize i = 0; i < kN; ++i) {
                    u32 v = 0;
                    p += proto::get_varint_fast(p, v);
                    sum += v;
                }
                g_sink = sum;
            });
        }
    }

    // ---------------------------------------------------------------- AES-CFB8
    {
        u8 key[16];
        for (auto& b : key) b = static_cast<u8>(rng());
        constexpr usize kLen = 16 * 1024;
        std::vector<std::vector<u8>> bufs(64, std::vector<u8>(kLen));
        std::vector<crypto::Cfb8> st(64);
        for (auto& s : st) s.init(key);

        bench("cfb8 encrypt, 1 stream", kLen, "MB/s", [&] { st[0].encrypt(bufs[0].data(), kLen); });
        bench("cfb8 decrypt, 1 stream (8 blocks parallel)", kLen, "MB/s", [&] { st[0].decrypt(bufs[0].data(), kLen); });
        for (usize n : {8u, 64u}) {
            std::vector<crypto::Cfb8Job> jobs;
            const std::string name = "cfb8 encrypt_batch, " + std::to_string(n) + " streams";
            bench(name.c_str(), static_cast<double>(kLen * n), "MB/s", [&] {
                jobs.clear();
                for (usize i = 0; i < n; ++i) jobs.push_back({&st[i], bufs[i].data(), kLen});
                crypto::cfb8_encrypt_batch(jobs);
            });
        }
    }

    // ---------------------------------------------------------------- framing
    {
        base::ByteBuf out(1 << 20);
        const proto::cb::KeepAlive ka{0x1122334455667788ull};
        bench("frame small packet, no compression", 1000, "M packets/s", [&] {
            out.clear();
            for (int i = 0; i < 1000; ++i) proto::write_packet(out, proto::kNoCompression, ka);
            g_sink = out.len;
        });
        bench("frame small packet, threshold 256", 1000, "M packets/s", [&] {
            out.clear();
            for (int i = 0; i < 1000; ++i) proto::write_packet(out, 256, ka);
            g_sink = out.len;
        });

        // A chunk-sized body with realistic entropy: paletted indices are low-entropy, not constant.
        std::vector<u8> chunk(24 * 1024);
        for (auto& b : chunk) b = static_cast<u8>((rng() % 8 == 0) ? rng() : rng() % 4);
        bench("frame 24 KiB body, deflate", static_cast<double>(chunk.size()), "MB/s", [&] {
            out.clear();
            proto::frame_raw(out, 256, chunk);
            g_sink = out.len;
        });
        out.clear();
        proto::frame_raw(out, 256, chunk);
        if (selected("frame 24 KiB body, deflate"))
            std::printf("%-44s %12.1f %%\n", "  (compressed size of that body)",
                        100.0 * out.len / static_cast<double>(chunk.size()));
    }
    world_benchmarks();
    entity_benchmarks();
    item_benchmarks();
    nbt_benchmarks();
    game_benchmarks();
    command_benchmarks();
    return 0;
}
