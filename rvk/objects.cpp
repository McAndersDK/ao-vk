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
}  // namespace

// The table rebuilt `size` long, without the entries not drawn for `keep` frames.
void Device::RehashObjects(size_t size, uint64_t keep)
{
    std::vector<ObjectEntry> old;
    old.swap(m_objects);
    m_objects.assign(size, ObjectEntry{});
    m_objectKeys.assign(size, 0);
    m_objectOrder.clear();                       // (entries' places: moved)
    m_objectOrderNext.clear();
    m_objectCount = 0;
    const size_t mask = size - 1;
    for (ObjectEntry& e : old) {
        if (!e.key || m_frameNumber - e.lastFrame > keep) continue;   // (keep ~0 when growing: all stay)
        size_t i = size_t(e.key) & mask;
        while (m_objectKeys[i]) i = (i + 1) & mask;
        m_objects[i] = e;
        m_objectKeys[i] = e.key;
        ++m_objectCount;
    }
}

// The current draw's entry (a static mesh: m_drawMeshKey, m_world), made if new.
Device::ObjectEntry* Device::FindObject()
{
    // The last frame's entry at this place in the order, if it is this object (exactly: mesh and matrix); the next
    // place's entry fetched meanwhile - an entry is the first touch of its object's memory this frame.
    const uint32_t pos = m_objectOrderPos++;
    if (pos + 1 < m_objectOrder.size()) {
        const char* next = reinterpret_cast<const char*>(&m_objects[m_objectOrder[pos + 1]]);
        for (size_t b = 0; b < sizeof(ObjectEntry); b += 64)
            _mm_prefetch(next + b, _MM_HINT_T0);
    }
    if (pos < m_objectOrder.size()) {
        ObjectEntry& e = m_objects[m_objectOrder[pos]];
        if (e.key && e.meshKey == m_drawMeshKey && std::memcmp(&e.world, &m_world, sizeof(m_world)) == 0) {
            if (e.lastFrame != m_frameNumber) {
                e.seenLastFrame = e.lastFrame + 1 == m_frameNumber;
                e.lastFrame = m_frameNumber;
            }
            m_objectOrderNext.push_back(m_objectOrder[pos]);
            ++m_objectHits;
            ++m_objectPredicted;
            return &e;
        }
    }
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
                e.boxExtent[j] = me[0] * std::fabs(m[0][j]) + me[1] * std::fabs(m[1][j]) + me[2] * std::fabs(m[2][j]);
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
            m_objectOrderNext.push_back(uint32_t(i));
            return &e;
        }
        if (keys[i] != k)
            continue;
        ObjectEntry& e = m_objects[i];
        if (e.meshKey == m_drawMeshKey && std::memcmp(&e.world, &m_world, sizeof(m_world)) == 0) {
            if (e.lastFrame != m_frameNumber) {  // (a second draw of it this frame - the ground's passes - changes nothing)
                e.seenLastFrame = e.lastFrame + 1 == m_frameNumber;
                e.lastFrame = m_frameNumber;
            }
            ++m_objectHits;
            m_objectOrderNext.push_back(uint32_t(i));
            return &e;
        }
    }
}

// Once a frame: every kObjectForget frames, the entries not drawn since then go (the table rebuilt without them).
void Device::PruneObjects()
{
    static const bool off = [] { const char* v = std::getenv("RANDYVK_OBJECT_CACHE"); return v && *v == '0'; }();
    static const bool verify = [] { const char* v = std::getenv("RANDYVK_OBJECT_VERIFY"); return v && *v == '1'; }();
    m_objectCache = !off;
    m_objectVerify = verify;
    m_drawObject = nullptr;
    m_objectOrder.swap(m_objectOrderNext);       // (the last frame's order; this frame's anew)
    m_objectOrderNext.clear();
    m_objectOrderPos = 0;
    if (m_frameNumber % kObjectForget == 0 && (m_objectHits || m_objectMisses)) {
        Log("object cache: %zu objects; %.0f draws a frame found theirs (%.0f in the last frame's order), %.0f new "
            "(last %llu frames)%s%s",
            m_objectCount, double(m_objectHits) / double(kObjectForget), double(m_objectPredicted) / double(kObjectForget),
            double(m_objectMisses) / double(kObjectForget), (unsigned long long)kObjectForget,
            m_objectVerify ? "; verified, mismatches: " : "", m_objectVerify ? std::to_string(m_objectMismatches).c_str() : "");
        m_objectHits = m_objectMisses = m_objectPredicted = 0;
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
