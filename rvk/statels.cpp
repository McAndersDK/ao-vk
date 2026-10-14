// Placed objects' shadows (RVK_StaticShad): every static object the playfield places - rocks, walls, buildings,
// trees - casts the sun's shadow from the playfield's own list, not from what the game draws. The game creates and
// hides those objects by distance rings and deletes the far ones (N3's statel grid), so a shadow made from its
// draws lost pieces whenever it culled or dropped them; remembering them (the caster cache) had to guess which draw
// was which piece - and two wall pieces turned round one pivot were taken for one (sun42, 2026-10-14).
//
// The list: ao-assets (python -m aoassets statels) reads each playfield's statel file (rdb 1000003) and writes
// <client>/randy-vk/statels/<playfield>.bin - the placements (each statel's frame matrix as N3 builds it from the
// file: yaw, pitch and roll as qZ * qX * qY, or a yaw with a stretch and a lean), and the meshes' shadow-casting
// pieces (no animated meshes: grass clumps, seaweed and swaying shrubs stay the live draws'; no frames named
// "invis", which the game hides), with alpha masks for cut-out pieces. Here: one buffer of their vertices and
// indices, every piece placed (its frame matrix in the mesh, then the statel's), a grid over the ground for the
// frame's range, and the pieces within the shadows' reach added to the shadow items each frame.
// They add to what the game draws (a depth map takes the same caster twice unharmed).
//
// Mode 1 (the default) casts only once the placement is confirmed: the first statel pieces the game draws are
// compared with ours (same vertex and index counts at the same spot: the same rotation and scale) - a decode gone
// wrong would throw shadows of things turned the wrong way. Confirmed once a session, logged; 2 casts at once.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

constexpr float kStatelCell = 64.0f;            // grid cells (world units)
// Matching draws that confirm the placement: of statels turned by yaw, pitch and roll; of stretched and leaned ones
// (flags & 1: fences and walls laid along a path - checked apart, as their decode is apart).
constexpr uint32_t kVerifyMatches[2] = {30, 10};
const char* const kKindName[2] = {"turned", "stretched (fences, walls)"};

#pragma pack(push, 1)
struct PackHeader {
    char magic[8];
    uint32_t version, playfield;
    uint32_t meshes, pieces, statels, textures, vertices, indices, texelBytes;
};
struct PackTexture { uint32_t type, id; uint16_t width, height; uint32_t texelOffset; };
struct PackMesh { uint32_t id, firstPiece, pieces; float boxMin[3], boxMax[3]; };
struct PackPiece {
    float local[16];
    uint32_t firstVertex, vertexCount, firstIndex, indexCount;
    int32_t texture;
    float alphaRef;
    float boxMin[3], boxMax[3];
};
struct PackStatel {
    float m[12];                                // its frame in the world: rows x, y, z, position (ao-assets)
    int16_t yaw, pitch, roll;                   // (for the log: degrees; pitch -32768: yaw in 630ths, a stretch)
    uint16_t mesh;
    float scale;
    uint32_t zone, ordinal;
};
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 44, "statel pack header");
static_assert(sizeof(PackTexture) == 16, "statel pack texture");
static_assert(sizeof(PackMesh) == 36, "statel pack mesh");
static_assert(sizeof(PackPiece) == 112, "statel pack piece");
static_assert(sizeof(PackStatel) == 68, "statel pack statel");

// A piece's identity for m_statelLookup: its counts and where it stands, to an eighth of a unit (the game's matrix
// and ours agree to a thousandth: offline against the game's frame dumps, 2026-10-14).
uint64_t PieceKey(uint32_t vertexCount, uint32_t indexCount, const float* at)
{
    uint64_t h = 0xCBF29CE484222325ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 0x100000001B3ull; };
    mix(vertexCount);
    mix(indexCount);
    for (int k = 0; k < 3; ++k) mix(uint64_t(int64_t(std::lround(double(at[k]) * 8.0))));
    return h;
}

}  // namespace

void Device::DestroyStatels()
{
    if (m_statelBuffer)
        vmaDestroyBuffer(m_allocator, m_statelBuffer, m_statelAllocation);
    m_statelBuffer = VK_NULL_HANDLE;
    m_statelAllocation = nullptr;
    for (Texture* t : m_statelMasks)
        if (t) DestroyTexture(t);
    m_statelMasks.clear();
    m_statelPieces.clear();
    m_statelLookup.clear();
    m_statelItems.clear();
    m_statelPlaced.clear();
    m_statelCellStart.clear();
    m_statelCellItems.clear();
    m_statelStamp.clear();
    m_statelPlayfield = 0;
}

void Device::SetStatelMode(uint32_t mode)
{
    if (mode != m_statelMode)
        Log("statels: mode %u (%s)", mode, mode == 0 ? "off" : mode == 1 ? "once the placement is confirmed" : "always");
    m_statelMode = mode;
}

// The playfield's pack (path "" or a missing file: none). Between frames (the game's playfield change).
bool Device::LoadStatels(const char* path, uint32_t playfield)
{
    if (playfield == m_statelPlayfield && !m_statelItems.empty())
        return true;
    if (m_statelBuffer || !m_statelMasks.empty()) {
        vkDeviceWaitIdle(m_device);              // (the old buffer may be in a frame still in flight)
        m_completed = std::max(m_completed, m_submitted);
    }
    DestroyStatels();
    if (!path || !*path)
        return false;
    std::FILE* f = std::fopen(path, "rb");
    if (!f) {
        Log("statels: playfield %u: no pack (%s) - its placed objects cast only as drawn", playfield, path);
        return false;
    }
    std::fseek(f, 0, SEEK_END);
    const long size = std::ftell(f);
    std::fseek(f, 0, SEEK_SET);
    std::vector<uint8_t> data(size > 0 ? size_t(size) : 0);
    const bool read = size > 0 && std::fread(data.data(), 1, data.size(), f) == data.size();
    std::fclose(f);
    PackHeader h{};
    if (!read || data.size() < sizeof(h)) {
        Log("statels: %s unreadable", path);
        return false;
    }
    std::memcpy(&h, data.data(), sizeof(h));
    // The sections, in order; each checked against the file's size.
    size_t at = sizeof(h);
    auto section = [&](size_t bytes) -> const uint8_t* {
        if (at + bytes > data.size()) return nullptr;
        const uint8_t* p = data.data() + at;
        at += bytes;
        return p;
    };
    if (std::memcmp(h.magic, "AOSTATEL", 8) != 0 || h.version != 2) {
        Log("statels: %s is not a statel pack of version 2 (ao-assets: python -m aoassets statels)", path);
        return false;
    }
    const auto* textures = reinterpret_cast<const PackTexture*>(section(size_t(h.textures) * sizeof(PackTexture)));
    const auto* meshes = reinterpret_cast<const PackMesh*>(section(size_t(h.meshes) * sizeof(PackMesh)));
    const auto* pieces = reinterpret_cast<const PackPiece*>(section(size_t(h.pieces) * sizeof(PackPiece)));
    const auto* statels = reinterpret_cast<const PackStatel*>(section(size_t(h.statels) * sizeof(PackStatel)));
    const uint8_t* vertices = section(size_t(h.vertices) * 20);
    const uint8_t* indices = section(size_t(h.indices) * 2);
    at = (at + 3) & ~size_t(3);
    const uint8_t* texels = section(h.texelBytes);
    if (!textures || !meshes || !pieces || !statels || !vertices || !indices || !texels) {
        Log("statels: %s is cut short", path);
        return false;
    }
    for (uint32_t i = 0; i < h.pieces; ++i) {
        const PackPiece& p = pieces[i];
        if (uint64_t(p.firstVertex) + p.vertexCount > h.vertices || uint64_t(p.firstIndex) + p.indexCount > h.indices ||
            p.texture >= int32_t(h.textures)) {
            Log("statels: %s: piece %u out of range", path, i);
            return false;
        }
    }

    // One buffer: the vertices (20 bytes: position, texture coordinates), then the indices. Written once by the CPU.
    const VkDeviceSize vertexBytes = VkDeviceSize(h.vertices) * 20, indexBytes = VkDeviceSize(h.indices) * 2;
    m_statelIndexBase = (vertexBytes + 63) & ~VkDeviceSize(63);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = std::max<VkDeviceSize>(m_statelIndexBase + indexBytes, 64);
    bi.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_SEQUENTIAL_WRITE_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
    VmaAllocationInfo info{};
    if (vmaCreateBuffer(m_allocator, &bi, &ac, &m_statelBuffer, &m_statelAllocation, &info) != VK_SUCCESS ||
        !info.pMappedData) {
        Log("statels: no buffer for %.1f MB", double(bi.size) / 1048576.0);
        if (m_statelBuffer) vmaDestroyBuffer(m_allocator, m_statelBuffer, m_statelAllocation);
        m_statelBuffer = VK_NULL_HANDLE;
        return false;
    }
    std::memcpy(info.pMappedData, vertices, size_t(vertexBytes));
    std::memcpy(static_cast<uint8_t*>(info.pMappedData) + m_statelIndexBase, indices, size_t(indexBytes));
    vmaFlushAllocation(m_allocator, m_statelAllocation, 0, VK_WHOLE_SIZE);

    // The cut-out pieces' alpha masks: white, the mask in alpha.
    std::vector<uint32_t> argb;
    for (uint32_t i = 0; i < h.textures; ++i) {
        const PackTexture& t = textures[i];
        Texture* tex = nullptr;
        if (t.width && t.height && uint64_t(t.texelOffset) + uint64_t(t.width) * t.height <= h.texelBytes) {
            argb.resize(size_t(t.width) * t.height);
            for (size_t k = 0; k < argb.size(); ++k)
                argb[k] = uint32_t(texels[t.texelOffset + k]) << 24 | 0xFFFFFFu;
            tex = CreateTexture(t.width, t.height, argb.data());
        }
        m_statelMasks.push_back(tex);
    }

    m_statelPieces.resize(h.pieces);
    for (uint32_t i = 0; i < h.pieces; ++i) {
        const PackPiece& p = pieces[i];
        StatelPiece& out = m_statelPieces[i];
        out.firstVertex = p.firstVertex;
        out.vertexCount = p.vertexCount;
        out.firstIndex = p.firstIndex;
        out.indexCount = p.indexCount;
        out.texture = p.texture >= 0 ? m_statelMasks[size_t(p.texture)] : nullptr;
        out.alphaRef = p.texture >= 0 && out.texture ? p.alphaRef : -1.0f;
    }

    // Every statel's pieces placed: piece -> mesh (its frames) -> world (the statel).
    float lo[2] = {1e30f, 1e30f}, hi[2] = {-1e30f, -1e30f};
    m_statelPlaced.reserve(h.statels);
    for (uint32_t s = 0; s < h.statels; ++s) {
        const PackStatel& st = statels[s];
        if (st.mesh >= h.meshes)
            continue;
        const PackMesh& mesh = meshes[st.mesh];
        if (uint64_t(mesh.firstPiece) + mesh.pieces > h.pieces)
            continue;
        d3d::Matrix placed{};
        for (int r = 0; r < 4; ++r)
            for (int c = 0; c < 3; ++c) placed.m[r][c] = st.m[r * 3 + c];
        placed.m[3][3] = 1.0f;
        m_statelPlaced.push_back({st.m[9], st.m[10], st.m[11], st.yaw, st.pitch, st.roll, st.scale, mesh.id});
        for (uint32_t k = 0; k < mesh.pieces; ++k) {
            const uint32_t pi = mesh.firstPiece + k;
            const PackPiece& p = pieces[pi];
            d3d::Matrix local;
            std::memcpy(&local, p.local, sizeof(local));
            StatelItem item;
            item.world = MulMatrix(local, placed);
            item.piece = pi;
            item.statel = uint32_t(m_statelPlaced.size() - 1);
            item.kind = st.pitch == -32768 ? 1 : 0;
            TransformBounds(p.boxMin, p.boxMax, item.world, item.boundsMin, item.boundsMax);
            lo[0] = std::min(lo[0], item.boundsMin[0]);
            lo[1] = std::min(lo[1], item.boundsMin[2]);
            hi[0] = std::max(hi[0], item.boundsMax[0]);
            hi[1] = std::max(hi[1], item.boundsMax[2]);
            m_statelItems.push_back(item);
        }
    }
    if (m_statelItems.empty()) {
        DestroyStatels();
        return false;
    }

    // The grid (x, z): each item in every cell its box covers - a mountain in many, a rock in one.
    m_statelGridOrigin[0] = std::floor(lo[0] / kStatelCell) * kStatelCell;
    m_statelGridOrigin[1] = std::floor(lo[1] / kStatelCell) * kStatelCell;
    m_statelGridW = std::min(4096u, uint32_t((hi[0] - m_statelGridOrigin[0]) / kStatelCell) + 1);
    m_statelGridH = std::min(4096u, uint32_t((hi[1] - m_statelGridOrigin[1]) / kStatelCell) + 1);
    auto cellRange = [&](const StatelItem& it, uint32_t* x0, uint32_t* x1, uint32_t* z0, uint32_t* z1) {
        auto cell = [&](float v, float origin, uint32_t n) {
            const float c = std::floor((v - origin) / kStatelCell);
            return uint32_t(std::clamp(c, 0.0f, float(n - 1)));
        };
        *x0 = cell(it.boundsMin[0], m_statelGridOrigin[0], m_statelGridW);
        *x1 = cell(it.boundsMax[0], m_statelGridOrigin[0], m_statelGridW);
        *z0 = cell(it.boundsMin[2], m_statelGridOrigin[1], m_statelGridH);
        *z1 = cell(it.boundsMax[2], m_statelGridOrigin[1], m_statelGridH);
    };
    const size_t cells = size_t(m_statelGridW) * m_statelGridH;
    m_statelCellStart.assign(cells + 1, 0);
    for (const StatelItem& it : m_statelItems) {
        uint32_t x0, x1, z0, z1;
        cellRange(it, &x0, &x1, &z0, &z1);
        for (uint32_t z = z0; z <= z1; ++z)
            for (uint32_t x = x0; x <= x1; ++x) ++m_statelCellStart[size_t(z) * m_statelGridW + x + 1];
    }
    for (size_t c = 0; c < cells; ++c) m_statelCellStart[c + 1] += m_statelCellStart[c];
    m_statelCellItems.resize(m_statelCellStart[cells]);
    std::vector<uint32_t> fill(m_statelCellStart.begin(), m_statelCellStart.end() - 1);
    for (uint32_t i = 0; i < m_statelItems.size(); ++i) {
        uint32_t x0, x1, z0, z1;
        cellRange(m_statelItems[i], &x0, &x1, &z0, &z1);
        for (uint32_t z = z0; z <= z1; ++z)
            for (uint32_t x = x0; x <= x1; ++x) m_statelCellItems[fill[size_t(z) * m_statelGridW + x]++] = i;
    }
    m_statelStamp.assign(m_statelItems.size(), 0);
    m_statelLookup.reserve(m_statelItems.size());
    for (const StatelItem& it : m_statelItems) {
        const StatelPiece& p = m_statelPieces[it.piece];
        m_statelLookup[PieceKey(p.vertexCount, p.indexCount, it.world.m[3])] |= uint8_t(1u << it.kind);
    }
    m_statelPlayfield = playfield;
    for (int k = 0; k < 2; ++k)
        if (m_statelVerified[k] == 0)
            m_statelMatched[k] = m_statelMismatched[k] = 0;
    auto state = [&](int k) {
        return m_statelVerified[k] > 0 ? "confirmed" : m_statelVerified[k] < 0 ? "REFUSED (see above)" : "to be confirmed";
    };
    Log("statels: playfield %u: %u placed, %zu pieces to cast (%u meshes, %u masks), %.1f MB; grid %ux%u; placement %s, "
        "of stretched ones %s", playfield, h.statels, m_statelItems.size(), h.meshes, h.textures,
        double(bi.size + VkDeviceSize(h.texelBytes) * 4) / 1048576.0, m_statelGridW, m_statelGridH, state(0), state(1));
    return true;
}

// A static draw of this frame (RecordShadowCaster), while the placement is unconfirmed: one of ours at the same spot
// with the same counts should have the draw's matrix.
void Device::CheckStatelDraw(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount)
{
    if ((m_statelVerified[0] != 0 && m_statelVerified[1] != 0) || m_statelItems.empty())
        return;
    const float x = world.m[3][0], y = world.m[3][1], z = world.m[3][2];
    const float cx = std::floor((x - m_statelGridOrigin[0]) / kStatelCell);
    const float cz = std::floor((z - m_statelGridOrigin[1]) / kStatelCell);
    if (cx < 0.0f || cz < 0.0f || cx >= float(m_statelGridW) || cz >= float(m_statelGridH))
        return;
    const size_t cell = size_t(cz) * m_statelGridW + size_t(cx);
    const StatelItem* sameSpot = nullptr;
    for (uint32_t k = m_statelCellStart[cell]; k < m_statelCellStart[cell + 1]; ++k) {
        const StatelItem& it = m_statelItems[m_statelCellItems[k]];
        const StatelPiece& p = m_statelPieces[it.piece];
        if (p.vertexCount != vertexCount || p.indexCount != indexCount)
            continue;
        const float dx = it.world.m[3][0] - x, dy = it.world.m[3][1] - y, dz = it.world.m[3][2] - z;
        if (dx * dx + dy * dy + dz * dz > 0.05f * 0.05f)
            continue;
        bool same = true;
        for (int r = 0; r < 3 && same; ++r)
            for (int c = 0; c < 3; ++c)
                if (std::fabs(it.world.m[r][c] - world.m[r][c]) > 0.02f * (1.0f + std::fabs(world.m[r][c]))) {
                    same = false;
                    break;
                }
        if (same) {
            ++m_statelMatched[it.kind];
            sameSpot = nullptr;
            break;
        }
        sameSpot = &it;
    }
    if (sameSpot) {
        const int kind = sameSpot->kind;
        if (++m_statelMismatched[kind] <= 4) {
            const StatelPlaced& s = m_statelPlaced[sameSpot->statel];
            const auto& a = world.m;
            const auto& b = sameSpot->world.m;
            Log("statels: a draw at (%.2f %.2f %.2f), mesh %u (yaw %d pitch %d roll %d, scale %.2f) turned otherwise "
                "than ours: drawn rows (%.3f %.3f %.3f) (%.3f %.3f %.3f) (%.3f %.3f %.3f), ours (%.3f %.3f %.3f) "
                "(%.3f %.3f %.3f) (%.3f %.3f %.3f)", double(x), double(y), double(z), s.mesh, s.yaw, s.pitch, s.roll,
                double(s.scale), a[0][0], a[0][1], a[0][2], a[1][0], a[1][1], a[1][2], a[2][0], a[2][1], a[2][2],
                b[0][0], b[0][1], b[0][2], b[1][0], b[1][1], b[1][2], b[2][0], b[2][1], b[2][2]);
        }
    }
    for (int k = 0; k < 2; ++k) {
        if (m_statelVerified[k] != 0)
            continue;
        if (m_statelMatched[k] >= kVerifyMatches[k] && m_statelMismatched[k] * 5 <= m_statelMatched[k]) {
            m_statelVerified[k] = 1;
            Log("statels: placement of %s statels confirmed - %u drawn pieces as ours (%u otherwise); they cast",
                kKindName[k], m_statelMatched[k], m_statelMismatched[k]);
        } else if (m_statelMismatched[k] >= kVerifyMatches[k] && m_statelMismatched[k] > m_statelMatched[k]) {
            m_statelVerified[k] = -1;
            Log("statels: placement of %s statels REFUSED - %u drawn pieces turned otherwise than ours (%u as ours); "
                "they don't cast (RVK_StaticShad=2 casts anyway)", kKindName[k], m_statelMismatched[k], m_statelMatched[k]);
        }
    }
}

// "turned confirmed (40 as ours, 0 otherwise), stretched unconfirmed (0, 0)" - for the log and the frame dump.
std::string Device::StatelPlacementState() const
{
    std::string out;
    for (int k = 0; k < 2; ++k) {
        char part[96];
        std::snprintf(part, sizeof(part), "%s%s %s (%u as ours, %u otherwise)", k ? ", " : "", k ? "stretched" : "turned",
                      m_statelVerified[k] > 0 ? "confirmed" : m_statelVerified[k] < 0 ? "refused" : "unconfirmed",
                      m_statelMatched[k], m_statelMismatched[k]);
        out += part;
    }
    return out;
}

// Whether placed pieces of a kind cast: mode 2 always; mode 1 once the turned ones' placement is confirmed (the
// stretched ones' too, for them).
bool Device::StatelKindCasting(int kind) const
{
    if (m_statelMode == 0)
        return false;
    return m_statelMode >= 2 || (m_statelVerified[0] > 0 && m_statelVerified[kind] > 0);
}

bool Device::StatelPieceCasting(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount) const
{
    if (m_statelLookup.empty())
        return false;
    auto found = m_statelLookup.find(PieceKey(vertexCount, indexCount, world.m[3]));
    if (found == m_statelLookup.end())
        return false;
    for (int k = 0; k < 2; ++k)
        if ((found->second >> k & 1) && StatelKindCasting(k))
            return true;
    return false;
}

// CollectShadowItems: the pieces within the shadows' reach of the eye.
void Device::AddStatelShadowItems()
{
    m_statelItemsThisFrame = 0;
    m_statelCoveredLast = m_statelCoveredThisFrame;   // (the frame's drawn pieces of ours: for the frame dump)
    m_statelCoveredThisFrame = 0;
    const bool casting[2] = {StatelKindCasting(0), StatelKindCasting(1)};
    if (m_statelItems.empty() || (!casting[0] && !casting[1]))
        return;
    UpdateFrameEye();
    const float range = CasterKeepRange();
    const float ex = m_frameEye[0], ez = m_frameEye[2];
    auto cell = [&](float v, float origin, uint32_t n) {
        return int(std::clamp(std::floor((v - origin) / kStatelCell), -1.0f, float(n)));
    };
    const int x0 = std::max(cell(ex - range, m_statelGridOrigin[0], m_statelGridW), 0);
    const int x1 = std::min(cell(ex + range, m_statelGridOrigin[0], m_statelGridW), int(m_statelGridW) - 1);
    const int z0 = std::max(cell(ez - range, m_statelGridOrigin[1], m_statelGridH), 0);
    const int z1 = std::min(cell(ez + range, m_statelGridOrigin[1], m_statelGridH), int(m_statelGridH) - 1);
    if (++m_statelStampNow == 0) {               // (wrapped: the stamps start over)
        std::fill(m_statelStamp.begin(), m_statelStamp.end(), 0u);
        m_statelStampNow = 1;
    }
    for (int z = z0; z <= z1; ++z)
        for (int x = x0; x <= x1; ++x) {
            const size_t c = size_t(z) * m_statelGridW + size_t(x);
            for (uint32_t k = m_statelCellStart[c]; k < m_statelCellStart[c + 1]; ++k) {
                const uint32_t i = m_statelCellItems[k];
                if (m_statelStamp[i] == m_statelStampNow)
                    continue;
                m_statelStamp[i] = m_statelStampNow;
                const StatelItem& s = m_statelItems[i];
                if (!casting[s.kind])
                    continue;
                // Its box's nearest point (x, z) within reach.
                const float dx = std::max({s.boundsMin[0] - ex, 0.0f, ex - s.boundsMax[0]});
                const float dz = std::max({s.boundsMin[2] - ez, 0.0f, ez - s.boundsMax[2]});
                if (dx * dx + dz * dz > range * range)
                    continue;
                const StatelPiece& p = m_statelPieces[s.piece];
                ShadowItem it;
                it.buffer = m_statelBuffer;
                it.ibBuffer = m_statelBuffer;
                it.vbOffset = VkDeviceSize(p.firstVertex) * 20;
                it.ibOffset = m_statelIndexBase + VkDeviceSize(p.firstIndex) * 2;
                it.primitive = 4;                 // (D3DPT_TRIANGLELIST)
                it.stride = 20;
                it.vertexCount = p.vertexCount;
                it.indexCount = p.indexCount;
                it.texture = p.texture;
                it.texOffset = p.texture ? 12 : -1;
                it.alphaRef = p.alphaRef;
                it.world = s.world;
                std::memcpy(it.boundsMin, s.boundsMin, sizeof(it.boundsMin));
                std::memcpy(it.boundsMax, s.boundsMax, sizeof(it.boundsMax));
                it.cached = true;
                it.animated = false;
                it.group = ~0u;
                it.owner = 0;
                it.kind = uint32_t(VisualKind::Static);
                it.leafKey = 0;
                std::memset(it.sway, 0, sizeof(it.sway));
                it.cacheKey = 0x5354000000000000ull | i;   // ("ST": a statel piece, for the frame dump)
                m_shadowItems.push_back(it);
                ++m_statelItemsThisFrame;
            }
        }
}

}  // namespace rvk
