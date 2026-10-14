// Mesh ids: which of the game's static meshes a draw is. ao-assets hashes every static mesh piece's geometry in
// rdb.db (aoassets/meshhash.py) into <client>/randy-vk/mesh-ids.bin; the same hash of a drawn piece (mesh_id.h, once
// per mesh: MeshInfo) finds the pieces it can be. A piece's geometry alone is often shared by several meshes (AO reuses
// pieces), so the meshes a game object (its visual) can be are those holding every piece it draws: the object's draws
// narrow it down (NoteMeshPiece; settled when the next object starts drawing).
//
// mesh-ids.bin (little-endian): "AOMESHID", u32 version 1, u32 entries, u32 names' bytes; the entries sorted by geo:
// u64 geo, u64 raw, u32 type, u32 id, u16 piece, u16 pieces, u32 name offset; then the names (UTF-8, NUL-terminated).
#include "internal.h"
#include "mesh_id.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <iterator>

namespace rvk {

using namespace detail;

namespace {
// The sky's scenery: meshes the game flies far out over the cities (kilometres off, past the fog's end and the far
// plane) - drawn whole and hazed as the far sky rather than cut off and fogged into flat shapes.
const char* const kSkyScenery[] = {"scenery_spaceship"};

#pragma pack(push, 1)
struct FileEntry {
    uint64_t geo, raw;
    uint32_t type, id;
    uint16_t piece, pieces;
    uint32_t name;
};
#pragma pack(pop)
static_assert(sizeof(FileEntry) == 32, "mesh-ids.bin entry");
}  // namespace

bool Device::LoadMeshIds(const char* path)
{
    m_meshIds.clear();
    m_meshIdNames.clear();
    FILE* f = std::fopen(path, "rb");
    if (!f) {
        Log("mesh ids: no %s (ao-assets: python -m aoassets mesh-ids)", path);
        return false;
    }
    char magic[8];
    uint32_t head[3];
    bool ok = std::fread(magic, 1, 8, f) == 8 && std::memcmp(magic, "AOMESHID", 8) == 0 &&
              std::fread(head, 4, 3, f) == 3 && head[0] == 1;
    if (ok) {
        std::vector<FileEntry> entries(head[1]);
        m_meshIdNames.resize(head[2]);
        ok = std::fread(entries.data(), sizeof(FileEntry), entries.size(), f) == entries.size() &&
             std::fread(m_meshIdNames.data(), 1, m_meshIdNames.size(), f) == m_meshIdNames.size();
        m_meshIds.reserve(entries.size());
        for (const FileEntry& e : entries)
            m_meshIds.push_back({e.geo, e.raw, e.type, e.id, e.piece, e.pieces, e.name});
    }
    std::fclose(f);
    if (!ok || !std::is_sorted(m_meshIds.begin(), m_meshIds.end(),
                               [](const MeshIdEntry& a, const MeshIdEntry& b) { return a.geo < b.geo; })) {
        Log("mesh ids: %s is not a mesh-ids.bin (version 1, sorted)", path);
        m_meshIds.clear();
        m_meshIdNames.clear();
        return false;
    }
    Log("mesh ids: %zu static mesh pieces from %s", m_meshIds.size(), path);
    return true;
}

const char* Device::MeshIdName(const MeshIdEntry& e) const
{
    return e.name < m_meshIdNames.size() ? m_meshIdNames.data() + e.name : "";
}

std::pair<const Device::MeshIdEntry*, const Device::MeshIdEntry*> Device::MeshIdsOf(uint64_t geo) const
{
    auto range = std::equal_range(m_meshIds.begin(), m_meshIds.end(), MeshIdEntry{geo},
                                  [](const MeshIdEntry& a, const MeshIdEntry& b) { return a.geo < b.geo; });
    return {m_meshIds.data() + (range.first - m_meshIds.begin()), m_meshIds.data() + (range.second - m_meshIds.begin())};
}

// A new mesh's ids (DrawMeshInfo): a static object's indexed triangles, positioned (XYZ), not skinned.
void Device::HashMeshId(MeshInfo& info, uint32_t fvf, uint32_t stride, const void* vertices, uint32_t vertexCount,
                        const uint16_t* indices, uint32_t indexCount)
{
    const VisualKind kind = VisualKind(m_drawVisualKind);
    if (m_meshIds.empty() || !vertices || !indices || indexCount < 3 || indexCount % 3 ||
        (kind != VisualKind::Static && kind != VisualKind::Room && kind != VisualKind::Unknown))
        return;
    const FvfLayout fl = DecodeFvf(fvf);
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    info.geo = meshid::Geo(v, stride, vertexCount, fl.offset[4], indices, indexCount);
    info.raw = meshid::Raw(fvf, v, stride, vertexCount, indices, indexCount);
    auto [first, last] = MeshIdsOf(info.geo);
    info.idCandidates = uint32_t(last - first);
    info.rawSame = false;
    info.skyScenery = false;
    for (const MeshIdEntry* e = first; e != last; ++e) {
        if (e->raw == info.raw) info.rawSame = true;
        for (const char* prefix : kSkyScenery)
            if (std::strncmp(MeshIdName(*e), prefix, std::strlen(prefix)) == 0) info.skyScenery = true;
    }
    if (info.skyScenery && m_meshIdStats.loggedSky < 8) {
        ++m_meshIdStats.loggedSky;
        Log("mesh id: '%s' is sky scenery: kept inside the far plane, fogged as the far sky", MeshIdName(*first));
    }
    ++m_meshIdStats.pieces;
    if (first != last) ++m_meshIdStats.known;
    if (info.rawSame) ++m_meshIdStats.rawSame;
    if (m_meshIdStats.loggedPieces < 40) {
        ++m_meshIdStats.loggedPieces;
        if (first == last)
            Log("mesh id: a piece of %u vertices, %u indices (fvf %#x): geo %016llx unknown", vertexCount, indexCount, fvf,
                (unsigned long long)info.geo);
        else
            Log("mesh id: a piece of %u vertices, %u indices: %u:%u '%s' piece %u of %u%s%s", vertexCount, indexCount,
                first->type, first->id, MeshIdName(*first), first->piece, first->pieces,
                last - first > 1 ? " (and other meshes)" : "", info.rawSame ? "" : " (vertex bytes differ: colours added?)");
    }
}

// This draw's piece, for the object (visual) drawing it: the meshes it can be narrowed by each of its pieces.
void Device::NoteMeshPiece()
{
    // (Its results feed the statistics and the log alone: worked out on one frame in 16 - the same objects get named,
    // a little later. Each object's lookup by its visual every frame was ~3% of the render thread over a city.)
    if (m_meshIds.empty() || (m_frameNumber & 15) != 0)
        return;
    // An object (visual) once: its result kept, its draws on later frames skip the work - the candidate lists built,
    // sorted and intersected per piece, every frame, cost the render thread ~2 ms over a city (4500 draws; user
    // 2026-10-14). (The results feed only the statistics and the log.)
    if (m_drawVisual != m_idObject.visual || m_frameNumber != m_idObject.frame) {
        SettleMeshObject();
        m_idObject.visual = m_drawVisual;
        m_idObject.frame = m_frameNumber;
        m_idObject.known = m_drawVisual && m_meshIdObjects.count(m_drawVisual) != 0;
    }
    if (m_idObject.known)
        return;
    const MeshInfo* info = m_drawMesh;
    if (!info || !info->geo || !m_drawVisual)
        return;
    ++m_idObject.pieces;
    auto [first, last] = MeshIdsOf(info->geo);
    std::vector<uint64_t> meshes;
    for (const MeshIdEntry* e = first; e != last; ++e)
        meshes.push_back(uint64_t(e->type) << 32 | e->id);
    std::sort(meshes.begin(), meshes.end());
    meshes.erase(std::unique(meshes.begin(), meshes.end()), meshes.end());
    if (m_idObject.pieces == 1) {
        m_idObject.candidates = std::move(meshes);
    } else {
        std::vector<uint64_t> both;
        std::set_intersection(m_idObject.candidates.begin(), m_idObject.candidates.end(), meshes.begin(), meshes.end(),
                              std::back_inserter(both));
        m_idObject.candidates = std::move(both);
    }
}

// The object whose draws ended: named if one mesh holds all its pieces (its result kept by its visual).
void Device::SettleMeshObject()
{
    if (m_idObject.pieces) {
        auto known = m_meshIdObjects.find(m_idObject.visual);
        const bool first = known == m_meshIdObjects.end();
        const size_t n = m_idObject.candidates.size();
        uint64_t mesh = n == 1 ? m_idObject.candidates.front() : 0;
        if (first) {
            ++m_meshIdStats.objects;
            if (n == 1) ++m_meshIdStats.named;
            else if (n > 1) ++m_meshIdStats.ambiguous;
            if (m_meshIdStats.loggedObjects < 60) {
                ++m_meshIdStats.loggedObjects;
                const char* name = "";
                for (const MeshIdEntry& e : m_meshIds)
                    if (n >= 1 && (uint64_t(e.type) << 32 | e.id) == m_idObject.candidates.front()) { name = MeshIdName(e); break; }
                if (n == 1)
                    Log("mesh id: object %08x = %u:%u '%s' (%u pieces drawn)", m_idObject.visual, uint32_t(mesh >> 32),
                        uint32_t(mesh), name, m_idObject.pieces);
                else if (n > 1)
                    Log("mesh id: object %08x = one of %zu meshes, e.g. %u:%u '%s' (%u pieces drawn)", m_idObject.visual, n,
                        uint32_t(m_idObject.candidates.front() >> 32), uint32_t(m_idObject.candidates.front()), name,
                        m_idObject.pieces);
                else
                    Log("mesh id: object %08x: no mesh holds all its %u pieces", m_idObject.visual, m_idObject.pieces);
            }
        }
        m_meshIdObjects[m_idObject.visual] = {mesh, uint32_t(n), m_frameNumber};
        if (m_meshIdObjects.size() > 50000) m_meshIdObjects.clear();
    }
    m_idObject = MeshIdObject{};
}

void Device::LogMeshIds()
{
    if (m_meshIds.empty())
        return;
    const auto& s = m_meshIdStats;
    Log("mesh ids: %llu static pieces seen, %llu (%.1f%%) known by their geometry (vertex bytes as in rdb.db: %llu); "
        "%llu objects: %llu named, %llu one of several, %llu unknown",
        (unsigned long long)s.pieces, (unsigned long long)s.known, s.pieces ? 100.0 * double(s.known) / double(s.pieces) : 0.0,
        (unsigned long long)s.rawSame, (unsigned long long)s.objects, (unsigned long long)s.named,
        (unsigned long long)s.ambiguous, (unsigned long long)(s.objects - s.named - s.ambiguous));
}

}  // namespace rvk
