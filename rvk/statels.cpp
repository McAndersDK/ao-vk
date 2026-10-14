// Placed objects' shadows (RVK_StaticShad): every static object the playfield places - rocks, walls, buildings,
// trees - casts the sun's shadow from the playfield's own list, not from what the game draws. The game creates and
// hides those objects by distance rings and deletes the far ones (N3's statel grid), so a shadow made from its
// draws lost pieces whenever it culled or dropped them; remembering them (the caster cache) had to guess which draw
// was which piece - and two wall pieces turned round one pivot were taken for one (sun42, 2026-10-14).
//
// The list: ao-assets (python -m aoassets statels) reads each playfield's statel file (rdb 1000003) and writes
// <client>/randy-vk/statels/<playfield>.bin - the placements, and the meshes' shadow-casting pieces (no animated
// meshes: grass clumps, seaweed and swaying shrubs stay the live draws'), with alpha masks for cut-out pieces. Here:
// one buffer of their vertices and indices, every piece placed (its world matrix as the game builds it: the
// statel's yaw, pitch and roll as qZ * qX * qY, scale, position; the piece's own frame matrix before), a grid over
// the ground for the frame's range, and the pieces within the shadows' reach added to the shadow items each frame.
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
constexpr uint32_t kVerifyMatches = 30;         // matching draws that confirm the placement

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
struct PackStatel { float pos[3]; int16_t yaw, pitch, roll; uint16_t mesh; float scale; uint32_t zone, ordinal; };
#pragma pack(pop)
static_assert(sizeof(PackHeader) == 44, "statel pack header");
static_assert(sizeof(PackTexture) == 16, "statel pack texture");
static_assert(sizeof(PackMesh) == 36, "statel pack mesh");
static_assert(sizeof(PackPiece) == 112, "statel pack piece");
static_assert(sizeof(PackStatel) == 32, "statel pack statel");

// Hamilton product a * b of (x, y, z, w) quaternions.
void QuatMul(const double* a, const double* b, double* out)
{
    out[0] = a[3] * b[0] + a[0] * b[3] + a[1] * b[2] - a[2] * b[1];
    out[1] = a[3] * b[1] - a[0] * b[2] + a[1] * b[3] + a[2] * b[0];
    out[2] = a[3] * b[2] + a[0] * b[1] - a[1] * b[0] + a[2] * b[3];
    out[3] = a[3] * b[3] - a[0] * b[0] - a[1] * b[1] - a[2] * b[2];
}

// A statel's matrix (row vectors) as N3 builds it (FUN_1002777a) and its frame takes it (RRefFrame_t: the quaternion's
// rotation, scaled, then the position): q = qZ(roll) * qX(pitch) * qY(yaw), each (axis sin(t / 2), cos(t / 2)).
d3d::Matrix StatelMatrix(const PackStatel& s)
{
    const double rad = 6.283185307179586 / 360.0;
    const double y = s.yaw * rad * 0.5, p = s.pitch * rad * 0.5, r = s.roll * rad * 0.5;
    const double qy[4] = {0.0, std::sin(y), 0.0, std::cos(y)};
    const double qx[4] = {std::sin(p), 0.0, 0.0, std::cos(p)};
    const double qz[4] = {0.0, 0.0, std::sin(r), std::cos(r)};
    double xy[4], q[4];
    QuatMul(qx, qy, xy);
    QuatMul(qz, xy, q);
    const double x2 = q[0] * 2.0, y2 = q[1] * 2.0, z2 = q[2] * 2.0;
    const double xx = q[0] * x2, yy = q[1] * y2, zz = q[2] * z2, xyv = q[1] * x2, xz = q[2] * x2, xw = x2 * q[3];
    const double yz = q[2] * y2, yw = y2 * q[3], zw = z2 * q[3];
    const double sc = s.scale;
    d3d::Matrix m{};
    m.m[0][0] = float((1.0 - yy - zz) * sc);
    m.m[0][1] = float((xyv + zw) * sc);
    m.m[0][2] = float((xz - yw) * sc);
    m.m[1][0] = float((xyv - zw) * sc);
    m.m[1][1] = float((1.0 - xx - zz) * sc);
    m.m[1][2] = float((yz + xw) * sc);
    m.m[2][0] = float((yw + xz) * sc);
    m.m[2][1] = float((yz - xw) * sc);
    m.m[2][2] = float((1.0 - xx - yy) * sc);
    m.m[3][0] = s.pos[0];
    m.m[3][1] = s.pos[1];
    m.m[3][2] = s.pos[2];
    m.m[3][3] = 1.0f;
    return m;
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
    if (std::memcmp(h.magic, "AOSTATEL", 8) != 0 || h.version != 1) {
        Log("statels: %s is not a statel pack (version 1)", path);
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
        const d3d::Matrix placed = StatelMatrix(st);
        m_statelPlaced.push_back({st.pos[0], st.pos[1], st.pos[2], st.yaw, st.pitch, st.roll, st.scale, mesh.id});
        for (uint32_t k = 0; k < mesh.pieces; ++k) {
            const uint32_t pi = mesh.firstPiece + k;
            const PackPiece& p = pieces[pi];
            d3d::Matrix local;
            std::memcpy(&local, p.local, sizeof(local));
            StatelItem item;
            item.world = MulMatrix(local, placed);
            item.piece = pi;
            item.statel = uint32_t(m_statelPlaced.size() - 1);
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
    m_statelPlayfield = playfield;
    if (m_statelVerified == 0)
        m_statelMatched = m_statelMismatched = 0;
    Log("statels: playfield %u: %u placed, %zu pieces to cast (%u meshes, %u masks), %.1f MB; grid %ux%u; placement %s",
        playfield, h.statels, m_statelItems.size(), h.meshes, h.textures,
        double(bi.size + VkDeviceSize(h.texelBytes) * 4) / 1048576.0, m_statelGridW, m_statelGridH,
        m_statelVerified > 0 ? "confirmed" : m_statelVerified < 0 ? "REFUSED (see above)" : "to be confirmed");
    return true;
}

// A static draw of this frame (RecordShadowCaster), while the placement is unconfirmed: one of ours at the same spot
// with the same counts should have the draw's matrix.
void Device::CheckStatelDraw(const d3d::Matrix& world, uint32_t vertexCount, uint32_t indexCount)
{
    if (m_statelVerified != 0 || m_statelItems.empty())
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
            ++m_statelMatched;
            sameSpot = nullptr;
            break;
        }
        sameSpot = &it;
    }
    if (sameSpot) {
        ++m_statelMismatched;
        if (m_statelMismatched <= 4) {
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
    if (m_statelMatched >= kVerifyMatches && m_statelMismatched * 5 <= m_statelMatched) {
        m_statelVerified = 1;
        Log("statels: placement confirmed - %u drawn pieces as ours (%u otherwise); the placed objects cast", m_statelMatched,
            m_statelMismatched);
    } else if (m_statelMismatched >= kVerifyMatches && m_statelMismatched > m_statelMatched) {
        m_statelVerified = -1;
        Log("statels: placement REFUSED - %u drawn pieces turned otherwise than ours (%u as ours); the placed objects "
            "don't cast (RVK_StaticShad=2 casts anyway)", m_statelMismatched, m_statelMatched);
    }
}

// CollectShadowItems: the pieces within the shadows' reach of the eye.
void Device::AddStatelShadowItems()
{
    m_statelItemsThisFrame = 0;
    if (m_statelItems.empty() || m_statelMode == 0 || (m_statelMode == 1 && m_statelVerified <= 0))
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
