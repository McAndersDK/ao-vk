// Mesh ids: a static mesh piece's geometry hashed as ao-assets hashes it from rdb.db (aoassets/meshhash.py) - the
// two must stay the same. The game draws a piece (SimpleMesh) as one triangle list of its whole vertex buffer, its
// vertices copied from rdb.db unchanged (randy31 RTriListSetVertices), its trilist's indices as they are; it may add
// colour columns to the vertices later (SimpleMesh::AddColours), so `geo` hashes only what that keeps:
//
//   geo = Fold(vertex count, index count, per vertex: x y z [u v of the first texture coordinates], the indices)
//   raw = Fold(fvf, vertex count, index count, the vertex bytes (as u32 words), the indices)
//
// Fold: h = h * P + w (mod 2^64) over u32 words (float bits as they are, indices widened), then fmix64(h ^ words).
#pragma once

#include <cstdint>
#include <cstring>

namespace rvk::meshid {

constexpr uint64_t kP = 0x9E3779B97F4A7C15ull;

inline uint64_t Fmix64(uint64_t h)
{
    h ^= h >> 33;
    h *= 0xFF51AFD7ED558CCDull;
    h ^= h >> 33;
    h *= 0xC4CEB9FE1A85EC53ull;
    h ^= h >> 33;
    return h;
}

struct Folder {
    uint64_t h = 0, n = 0;
    void Add(uint32_t w) { h = h * kP + w; ++n; }
    uint64_t Done() const { return Fmix64(h ^ n); }
};

// texOffset: the first texture coordinates' offset in the vertex (bytes), or < 0 for none.
inline uint64_t Geo(const uint8_t* vertices, uint32_t stride, uint32_t count, int texOffset, const uint16_t* indices,
                    uint32_t indexCount)
{
    Folder f;
    f.Add(count);
    f.Add(indexCount);
    for (uint32_t i = 0; i < count; ++i) {
        const uint8_t* v = vertices + size_t(i) * stride;
        uint32_t w[5];
        std::memcpy(w, v, 12);
        int n = 3;
        if (texOffset >= 0) { std::memcpy(w + 3, v + texOffset, 8); n = 5; }
        for (int k = 0; k < n; ++k) f.Add(w[k]);
    }
    for (uint32_t i = 0; i < indexCount; ++i) f.Add(indices[i]);
    return f.Done();
}

inline uint64_t Raw(uint32_t fvf, const uint8_t* vertices, uint32_t stride, uint32_t count, const uint16_t* indices,
                    uint32_t indexCount)
{
    Folder f;
    f.Add(fvf);
    f.Add(count);
    f.Add(indexCount);
    const size_t words = size_t(count) * stride / 4;
    for (size_t i = 0; i < words; ++i) {
        uint32_t w;
        std::memcpy(&w, vertices + i * 4, 4);
        f.Add(w);
    }
    for (uint32_t i = 0; i < indexCount; ++i) f.Add(indices[i]);
    return f.Done();
}

}  // namespace rvk::meshid
