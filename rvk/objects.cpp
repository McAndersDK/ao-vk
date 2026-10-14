// Per-object frame cache (docs/render-batching-plan.md P1). A static mesh drawn at a world matrix is one of the game's
// objects; over a city ~3,000 of them are drawn every frame, each where it was the frame before. What a draw works out
// from its mesh and matrix alone - the shadow caster's key and box, whether one of our placed objects casts for it,
// the grass cover's record, whether it stood there last frame (its motion) - is kept with the object, found by one
// hash of the two, instead of worked out again every frame.
// Found anew (a new entry) whenever either changes: a moving object is a new entry every frame and takes the full
// path. Entries not drawn for 600 frames go (PruneObjects).
#include "internal.h"

#include <cmath>
#include <xmmintrin.h>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rvk {

using namespace detail;

namespace {
constexpr uint64_t kObjectForget = 600;          // frames undrawn before an entry goes
constexpr size_t kObjectMany = size_t(1) << 16;  // past this many, entries go after a tenth of that
constexpr size_t kObjectCandidates = 16384;      // (a power of two)
}  // namespace

// The table rebuilt `size` long, without the entries not drawn for `keep` frames.
void Device::RehashObjects(size_t size, uint64_t keep)
{
    std::vector<ObjectEntry> old;
    old.swap(m_objects);
    m_objects.assign(size, ObjectEntry{});
    m_objectKeys.assign(size, 0);
    m_objectPrev = ~0u;                          // (entries' places moved: their successor links go)
    m_objectCount = 0;
    const size_t mask = size - 1;
    for (ObjectEntry& e : old) {
        if (!e.key || m_frameNumber - e.lastFrame > keep) continue;   // (keep ~0 when growing: all stay)
        size_t i = size_t(e.key) & mask;
        while (m_objectKeys[i]) i = (i + 1) & mask;
        m_objects[i] = e;
        m_objects[i].next = ~0u;
        m_objectKeys[i] = e.key;
        ++m_objectCount;
    }
}

// The current draw's entry (a static mesh: m_drawMeshKey, m_world), made if new.
Device::ObjectEntry* Device::FindObject()
{
    // The entry that followed the last one found, the frame before (its `next`), if it is this object (exactly: mesh
    // and matrix) - the game draws its scene in much the same order frame after frame; an object coming into or going
    // out of view costs the lookups around it only. (A place in the frame's order went wrong for the rest of the
    // frame after one such change.)
    uint32_t index = ~0u;
    if (m_objectPrev != ~0u) {
        const uint32_t n = m_objects[m_objectPrev].next;
        if (n != ~0u) {
            const ObjectEntry& e = m_objects[n];
            if (e.key && e.meshKey == m_drawMeshKey && std::memcmp(&e.world, &m_world, sizeof(m_world)) == 0) {
                index = n;
                ++m_objectPredicted;
            }
        }
    }
    if (index == ~0u) {
        uint64_t k = m_drawMeshKey ^ 0x9E3779B97F4A7C15ull;
        uint32_t w[16];
        std::memcpy(w, &m_world, sizeof(w));
        for (int i = 0; i < 16; i += 2) {
            k = (k ^ (uint64_t(w[i]) | uint64_t(w[i + 1]) << 32)) * 0xFF51AFD7ED558CCDull;
            k ^= k >> 32;
        }
        if (!k) k = 1;
        if ((m_objectCount + 1) * 2 > m_objects.size())   // at most half full
            RehashObjects(std::max<size_t>(m_objects.size() * 2, 4096), ~uint64_t(0));
        const size_t mask = m_objects.size() - 1;
        const uint64_t* keys = m_objectKeys.data();
        for (size_t i = size_t(k) & mask;; i = (i + 1) & mask) {
            if (!keys[i]) {
                // New: an entry only for one seen at this matrix last frame too (a candidate then).
                // (Probed a few places on: candidates live two frames, a slot older than that is free. Direct-mapped,
                // two still objects in one slot pushed each other out every frame and never got entries.)
                if (m_objectCandidates.empty()) m_objectCandidates.resize(kObjectCandidates);
                ObjectCandidate* cand = nullptr;
                ObjectCandidate* free = nullptr;
                for (size_t c = 0; c < 16; ++c) {
                    ObjectCandidate& s = m_objectCandidates[(size_t(k) + c) & (kObjectCandidates - 1)];
                    if (s.key == k) { cand = &s; break; }
                    if (!free && (s.key == 0 || s.frame + 1 < m_frameNumber)) free = &s;
                }
                if (!cand || cand->frame + 1 != m_frameNumber) {
                    if (cand && cand->frame != m_frameNumber) *cand = {k, m_frameNumber};
                    else if (!cand && free) *free = {k, m_frameNumber};
                    ++m_objectPassing;
                    return nullptr;
                }
                ObjectEntry& e = m_objects[i];
                e = ObjectEntry{};
                e.key = k;
                m_objectKeys[i] = k;
                e.meshKey = m_drawMeshKey;
                e.world = m_world;
                e.lastFrame = m_frameNumber;
                // Its world boxes, as DrawWorldBox and FrameLightMask work them out (the same sums: the same bits).
                const auto& m = m_world.m;
                const float* mn = m_drawMesh->boundsMin;
                const float* mx = m_drawMesh->boundsMax;
                float mc[3], me[3];
                for (int j = 0; j < 3; ++j) {
                    mc[j] = 0.5f * (mn[j] + mx[j]);
                    me[j] = 0.5f * (mx[j] - mn[j]);
                }
                for (int j = 0; j < 3; ++j) {
                    e.boxCentre[j] = mc[0] * m[0][j] + mc[1] * m[1][j] + mc[2] * m[2][j] + m[3][j];
                    e.boxExtent[j] =
                        me[0] * std::fabs(m[0][j]) + me[1] * std::fabs(m[1][j]) + me[2] * std::fabs(m[2][j]);
                    float c = m[3][j], x = 0.0f;
                    for (int a = 0; a < 3; ++a) {
                        c += 0.5f * (mn[a] + mx[a]) * m[a][j];
                        x += 0.5f * (mx[a] - mn[a]) * std::fabs(m[a][j]);
                    }
                    e.lightMin[j] = c - x;
                    e.lightMax[j] = c + x;
                }
                ++m_objectCount;
                ++m_objectMisses;
                index = uint32_t(i);
                break;
            }
            if (keys[i] != k)
                continue;
            const ObjectEntry& e = m_objects[i];
            if (e.meshKey == m_drawMeshKey && std::memcmp(&e.world, &m_world, sizeof(m_world)) == 0) {
                index = uint32_t(i);
                break;
            }
        }
    }
    ObjectEntry& e = m_objects[index];
    if (e.lastFrame != m_frameNumber) {          // (a second draw of it this frame - the ground's passes: nothing)
        e.seenLastFrame = e.lastFrame + 1 == m_frameNumber;
        e.lastFrame = m_frameNumber;
    }
    ++m_objectHits;
    if (m_objectPrev != ~0u && m_objectPrev != index)
        m_objects[m_objectPrev].next = index;    // (the order as of this frame, for the next)
    m_objectPrev = index;
    if (e.next != ~0u) {                         // its successor's memory fetched meanwhile (an entry is a few lines)
        const char* next = reinterpret_cast<const char*>(&m_objects[e.next]);
        for (size_t b = 0; b < sizeof(ObjectEntry); b += 64)
            _mm_prefetch(next + b, _MM_HINT_T0);
    }
    return &e;
}

// Once a frame: every kObjectForget frames, the entries not drawn since then go (the table rebuilt without them).
void Device::PruneObjects()
{
    static const bool off = [] { const char* v = std::getenv("RANDYVK_OBJECT_CACHE"); return v && *v == '0'; }();
    static const bool verify = [] { const char* v = std::getenv("RANDYVK_OBJECT_VERIFY"); return v && *v == '1'; }();
    m_objectCache = !off;
    m_objectVerify = verify;
    m_drawObject = nullptr;
    m_objectPrev = ~0u;                          // (a frame's first lookup has no predecessor)
    if (m_frameNumber % kObjectForget == 0 && (m_objectHits || m_objectMisses)) {
        Log("object cache: %zu objects; %.0f draws a frame found theirs (%.0f by the last frame's order), %.0f new, "
            "%.0f not still (last %llu frames)%s%s",
            m_objectCount, double(m_objectHits) / double(kObjectForget),
            double(m_objectPredicted) / double(kObjectForget),
            double(m_objectMisses) / double(kObjectForget), double(m_objectPassing) / double(kObjectForget),
            (unsigned long long)kObjectForget,
            m_objectVerify ? "; verified, mismatches: " : "",
            m_objectVerify ? std::to_string(m_objectMismatches).c_str() : "");
        m_objectHits = m_objectMisses = m_objectPredicted = m_objectPassing = 0;
    }
    const bool many = m_objectCount > kObjectMany;
    if (m_objectCount == 0 || (m_frameNumber % kObjectForget != 0 && !many))
        return;
    RehashObjects(m_objects.size(), many ? kObjectForget / 10 : kObjectForget);
}

// StatelPieceCasting for the current draw: the lookup kept with its object (the pack's pieces don't change while it is
// loaded); whether our pieces of that kind cast is asked every time (it changes as their placement is confirmed).
bool Device::DrawStatelCasting(uint32_t vertexCount, uint32_t indexCount)
{
    uint8_t kinds;
    if (m_drawObject) {
        ObjectEntry& o = *m_drawObject;
        if (o.statelGen != m_statelLookupGen) {
            o.statelKinds = StatelPieceKinds(m_world, vertexCount, indexCount);
            o.statelGen = m_statelLookupGen;
        }
        kinds = o.statelKinds;
    } else {
        kinds = StatelPieceKinds(m_world, vertexCount, indexCount);
    }
    for (int k = 0; k < 2; ++k)
        if ((kinds >> k & 1) && StatelKindCasting(k))
            return true;
    return false;
}

}  // namespace rvk
