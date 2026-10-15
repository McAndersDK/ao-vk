// Leaves on trees, shrubs and palms (RVK_LeafOn): an addition on top of the game's canopies, which are a few big cards
// with a painted cluster of leaves and twigs on each (a 20-unit jungle crown is 54 triangles).
//
// A canopy is a lit, static mesh drawn into the scene with a texture that is mostly holes (CanopyKind). Its leaves are
// baked once per mesh and texture (BakeLeaves): sprigs scattered over its cards where the texture is leaf, each a small
// card of its own cut from the same texture - the painted leaves around that spot - tilted and turned its own way and
// lifted off the card a little, so the crown gets depth. They go into one GPU pool, shuffled so that any first part of
// a canopy's leaves is an even thinning of all of them (the distance level of detail draws a first part).
//
// Drawn just before the canopy's own draw (DrawLeaves), with the canopy's record: the same world matrix, render state,
// lights and texture; leaf.vert (ffp.vert built with RVK_LEAF) makes each leaf's vertices from its record and passes
// them through the canopy's own vertex path. The canopy's cards and its leaves sway together with the branches
// (ffp.vert BranchSway, D.leaf), and each leaf flutters on its own.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

// One leaf in the pool (ffp.vert RVK_LEAF: Leaves).
struct LeafRecord {
    float centre[3];
    uint32_t halfUxy;            // half its width along the texture's u, model space (half floats x, y)
    uint32_t halfUzVx;           // ... z, half its height along v x
    uint32_t halfVyz;            // ... y, z
    uint32_t uv;                 // its centre in the texture (unorm16 x 2)
    uint32_t uvHalfPhase;        // its half size in the texture (unorm12 x 0.25, u then v), its phase (unorm8)
};
static_assert(sizeof(LeafRecord) == 32, "leaf record (two uvec4)");

constexpr uint32_t kMaxLeavesPerSet = 16384;
constexpr uint32_t kBakesPerFrame = 4;           // canopies baked a frame at most (the rest wait for the next)
constexpr uint64_t kLeafSetIdleFrames = 3600;    // a set not drawn for this long leaves the pool

uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t m = x & 0x7FFFFFu;
    if (e <= 0) {                                // tiny: zero (a leaf's extent is never that small)
        return uint16_t(sign);
    }
    if (e >= 31)
        return uint16_t(sign | 0x7BFFu);         // the largest finite half
    uint32_t h = sign | (uint32_t(e) << 10) | (m >> 13);
    if (m & 0x1000u)                             // round to nearest
        ++h;
    return uint16_t(h);
}
uint32_t Halves(float a, float b) { return uint32_t(ToHalf(a)) | uint32_t(ToHalf(b)) << 16; }

// A small, stable random generator: the same mesh bakes the same leaves (the temporal anti-aliasing and the distance
// thinning rely on a leaf staying put).
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed * 0x9E3779B97F4A7C15ull + 0x632BE59BD9B4E019ull) {}
    uint32_t Next()
    {
        s ^= s >> 12;
        s ^= s << 25;
        s ^= s >> 27;
        return uint32_t((s * 0x2545F4914F6CDD1Dull) >> 32);
    }
    float Unit() { return float(Next() >> 8) * (1.0f / 16777216.0f); }
};

struct V3 { float x, y, z; };
V3 operator+(V3 a, V3 b) { return {a.x + b.x, a.y + b.y, a.z + b.z}; }
V3 operator-(V3 a, V3 b) { return {a.x - b.x, a.y - b.y, a.z - b.z}; }
V3 operator*(V3 a, float s) { return {a.x * s, a.y * s, a.z * s}; }
float Dot(V3 a, V3 b) { return a.x * b.x + a.y * b.y + a.z * b.z; }
V3 Cross(V3 a, V3 b) { return {a.y * b.z - a.z * b.y, a.z * b.x - a.x * b.z, a.x * b.y - a.y * b.x}; }
float Length(V3 a) { return std::sqrt(Dot(a, a)); }
V3 Normalize(V3 a) { float l = Length(a); return l > 1e-12f ? a * (1.0f / l) : V3{0, 1, 0}; }
// v turned by `angle` about the unit axis k (Rodrigues).
V3 Rotate(V3 v, V3 k, float angle)
{
    float c = std::cos(angle), s = std::sin(angle);
    return v * c + Cross(k, v) * s + k * (Dot(k, v) * (1.0f - c));
}

// A solid mesh's axis (a tree trunk's), model space: up its length (`k`: the model axis that stands up) in ten bands,
// each band's middle - the vertices near the band below's middle only, so a limb reaching out doesn't pull it aside
// (the first band: all of the lowest tenth). x y z a point; stops where the wood ends.
void TraceAxis(const uint8_t* v, uint32_t stride, uint32_t n, int k, std::vector<float>& out)
{
    out.clear();
    if (!v || n < 8)
        return;
    const int a = (k + 1) % 3, b = (k + 2) % 3;
    auto at = [&](uint32_t i, float p[3]) { std::memcpy(p, v + size_t(i) * stride, 12); };
    float lo = 1e30f, hi = -1e30f;
    for (uint32_t i = 0; i < n; ++i) {
        float p[3];
        at(i, p);
        lo = std::min(lo, p[k]);
        hi = std::max(hi, p[k]);
    }
    const int bands = 10;
    const float step = (hi - lo) / float(bands);
    if (!(step > 1e-4f))
        return;
    float cx = 0.0f, cz = 0.0f, r = 0.0f;
    for (int band = 0; band < bands; ++band) {
        const float b0 = lo + float(band) * step, b1 = b0 + step;
        float sx = 0.0f, sz = 0.0f, near2 = 1e30f, nx = 0.0f, nz = 0.0f;
        uint32_t count = 0;
        const float reach = band == 0 ? 1e30f : std::max(2.5f * r, 0.15f * step);
        for (uint32_t i = 0; i < n; ++i) {
            float p[3];
            at(i, p);
            if (p[k] < b0 || p[k] > b1)
                continue;
            const float d2 = (p[a] - cx) * (p[a] - cx) + (p[b] - cz) * (p[b] - cz);
            if (d2 < near2) { near2 = d2; nx = p[a]; nz = p[b]; }
            if (band == 0 || d2 <= reach * reach) { sx += p[a]; sz += p[b]; ++count; }
        }
        if (!count) {                             // (the wood moved further than the reach: from the nearest vertex)
            if (near2 >= 1e30f)
                break;
            sx = nx; sz = nz; count = 1;
        }
        cx = sx / float(count);
        cz = sz / float(count);
        float spread = 0.0f;
        uint32_t m = 0;
        for (uint32_t i = 0; i < n; ++i) {
            float p[3];
            at(i, p);
            if (p[k] < b0 || p[k] > b1)
                continue;
            const float d = std::hypot(p[a] - cx, p[b] - cz);
            if (band == 0 || d <= 3.0f * std::max(r, 0.05f)) { spread += d; ++m; }
        }
        r = m ? std::max(spread / float(m), 0.05f) : r;
        float q[3];
        q[a] = cx; q[b] = cz; q[k] = 0.5f * (b0 + b1);
        out.insert(out.end(), q, q + 3);
    }
}

// A seed from the mesh's own content (its positions and indices): the same tree bakes the same leaves and branches every
// time (m_drawMeshKey holds a static snapshot's serial - a new one each upload: the branches changed when the leaves
// were turned off and on).
uint64_t ContentSeed(const void* vertices, uint32_t stride, uint32_t vertexCount, const uint16_t* indices,
                     uint32_t indexCount)
{
    uint64_t h = 0xCBF29CE484222325ull ^ vertexCount ^ (uint64_t(indexCount) << 32);
    auto mix = [&h](uint64_t x) { h = (h ^ x) * 0x100000001B3ull; h ^= h >> 29; };
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        uint32_t p[3];
        std::memcpy(p, v + size_t(i) * stride, 12);
        mix(p[0]); mix(p[1]); mix(p[2]);
    }
    for (uint32_t i = 0; indices && i < indexCount; ++i)
        mix(indices[i]);
    return h;
}

// The texture's alpha (0..1) at (u, v), filtered between the four nearest cells of its mask (wrapping).
float MaskAlpha(const std::vector<uint8_t>& mask, uint32_t w, uint32_t h, float u, float v)
{
    float x = (u - std::floor(u)) * float(w) - 0.5f, y = (v - std::floor(v)) * float(h) - 0.5f;
    float fx = std::floor(x), fy = std::floor(y);
    int x0 = int(fx), y0 = int(fy);
    float tx = x - fx, ty = y - fy;
    auto at = [&](int i, int j) {
        i = ((i % int(w)) + int(w)) % int(w);
        j = ((j % int(h)) + int(h)) % int(h);
        return float(mask[size_t(j) * w + size_t(i)]);
    };
    float a = at(x0, y0) * (1 - tx) + at(x0 + 1, y0) * tx;
    float b = at(x0, y0 + 1) * (1 - tx) + at(x0 + 1, y0 + 1) * tx;
    return (a * (1 - ty) + b * ty) / 255.0f;
}

}  // namespace

// Whether the current draw is a canopy, and which kind: lit, static vertices, drawn into the scene with depth writes and
// a texture that is mostly holes (its mean alpha well under the walls' and signs'), standing upright (the model axis
// most aligned with the world's up), not one flat plane (a sign, a poster). Shrubs are plant-sized (the plants that
// sway: 0.2 - 3 units tall), trees up to 45 units.
Device::Canopy Device::CanopyKind(uint32_t primitive, uint32_t fvf, uint32_t stride, const void* vertices,
                                  uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    Texture* tex = m_textures[0];
    if (m_target != m_scene || !tex || tex->m_alphaMask.empty() || !m_rs[d3d::RS_LIGHTING] || m_drawIsLabel ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || IsTerrain(fvf) || !m_rs[d3d::RS_ZWRITEENABLE] ||
        !(m_rs[d3d::RS_ALPHATESTENABLE] || m_rs[d3d::RS_ALPHABLENDENABLE]) || vertexCount < 3 || vertexCount > 4096 ||
        !m_drawMesh || primitive < d3d::TriangleList || primitive > d3d::TriangleFan || !vertices ||
        DecodeFvf(fvf).offset[4] < 0)
        return Canopy::None;
    // Mostly holes (leaf textures: mean alpha 0.3 - 0.45 in the game's trees).
    uint32_t sum = 0;
    for (uint8_t a : tex->m_alphaMask) sum += a;
    const float mean = float(sum) / (255.0f * float(tex->m_alphaMask.size()));
    if (mean < 0.05f || mean > 0.65f)
        return Canopy::None;
    const auto& w = m_world.m;
    int axis = 0;
    float best = -1.0f, scale = 0.0f;
    for (int i = 0; i < 3; ++i) {
        float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        float up = len > 0.0f ? std::fabs(w[i][1]) / len : 0.0f;
        if (up > best) { best = up; axis = i; scale = len; }
    }
    if (best < 0.7f)
        return Canopy::None;
    const float height = (m_drawMesh->boundsMax[axis] - m_drawMesh->boundsMin[axis]) * scale;
    float across = 0.0f;
    for (int i = 0; i < 3; ++i) {
        if (i == axis)
            continue;
        const float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
        across = std::max(across, (m_drawMesh->boundsMax[i] - m_drawMesh->boundsMin[i]) * len);
    }
    if (height < 0.2f || height > 45.0f || across > 60.0f || (height <= 3.0f && across > 6.0f))
        return Canopy::None;
    if (m_drawMesh->flatShape < 0)
        m_drawMesh->flatShape = OnePlane(primitive, stride, vertices, vertexCount, indices, indexCount) ? 1 : 0;
    if (m_drawMesh->flatShape == 1)
        return Canopy::None;
    // A palm's (or a fern's) fronds: each card one long frond, its texture at least 2.5 times as long as wide.
    const uint32_t lo = std::min(tex->m_width, tex->m_height), hi = std::max(tex->m_width, tex->m_height);
    if (height > 1.0f && hi >= 5 * lo / 2)
        return Canopy::Palm;
    return height <= 3.0f ? Canopy::Shrub : Canopy::Tree;
}

// Per canopy draw (Draw, before its ring space is reserved): its leaf set, baked if it isn't yet (or its texture or the
// settings changed), and how many of its leaves to draw at this distance. Sets m_drawLeaves / m_drawLeafCount.
void Device::CanopyParams(uint32_t primitive, uint32_t fvf, const FvfLayout& layout, const void* vertices,
                          uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    m_drawLeaves = nullptr;
    m_drawLeafKey = 0;
    m_drawLeafCount = 0;
    m_drawLeafFall = 0;
    if (!m_leaf.on || m_external || m_drawGpu || !m_drawMesh || !m_leafPipelines[0] || !m_drawMeshStatic ||
        m_drawMesh->firstFrame + 2 > m_frameNumber || m_target != m_scene)
        return;
    Texture* tex = m_textures[0];
    if (!tex || tex->m_alphaMask.empty() || !m_rs[d3d::RS_LIGHTING] || !m_rs[d3d::RS_ZWRITEENABLE])
        return;                                  // (most draws: not worth a look-up)
    const uint64_t key = m_drawMeshKey ^ (uint64_t(reinterpret_cast<uintptr_t>(tex)) * 0x9E3779B97F4A7C15ull);
    auto it = m_leafSets.find(key);
    if (it == m_leafSets.end()) {
        LeafSet set;
        set.kind = CanopyKind(primitive, fvf, layout.stride, vertices, vertexCount, indices, indexCount);
        it = m_leafSets.emplace(key, set).first;
    }
    LeafSet& set = it->second;
    set.lastFrame = m_frameNumber;
    if (set.kind == Canopy::None)
        return;
    ++m_canopyCount[uint32_t(set.kind)];
    if ((set.kind == Canopy::Tree && !m_leaf.trees) || (set.kind == Canopy::Shrub && !m_leaf.shrubs) ||
        (set.kind == Canopy::Palm && !m_leaf.palms))
        return;
    if (set.kind == Canopy::Palm) {              // no leaves: its fronds bend (ffp.vert BranchSway, D.leaf.w < 0)
        if (!set.baked) {
            CrownOf(set);
            set.baked = true;
        }
        m_drawLeaves = &set;
        m_drawLeafKey = key;
        return;
    }
    // A tree with its own leaf art (RVK_LeafOwn): baked from it (again when the art changes - hot reload).
    Texture* art = set.kind == Canopy::Tree && m_leaf.own ? tex->m_leafArt : nullptr;
    if (art && set.trunk.empty()) {              // its trunk: a solid draw just before, or (NoteSolidDraw) just after
        float c[3], e[3];
        DrawWorldBox(c, e);
        RecentCanopy& r = m_recentCanopy;
        r.setKey = key;
        r.world = m_world;
        for (int j = 0; j < 3; ++j) { r.lo[j] = c[j] - e[j]; r.hi[j] = c[j] + e[j]; }
        r.ownDraw = m_frameDraw;
        for (const SolidBox& b : m_recentSolid)
            if (b.draw && m_frameDraw - b.draw <= 4 && TrunkUnder(b, r.lo, r.hi)) {
                GiveTrunk(key, m_world, b);
                break;
            }
    }
    if (!set.baked || set.texVersion != tex->m_alphaVersion || set.settingsVersion != m_leafVersion || set.art != art ||
        (art && set.artVersion != tex->m_leafArtVersion)) {
        if (m_leafBakesThisFrame >= kBakesPerFrame)
            return;                              // next frame
        ++m_leafBakesThisFrame;
        set.art = art;
        set.artVersion = art ? tex->m_leafArtVersion : 0;
        if (!BakeLeaves(set, primitive, layout, vertices, vertexCount, indices, indexCount))
            return;
    }
    if (!set.count)
        return;
    // The distance: full leaves up to half of RVK_LeafDist, then thinning out (the shuffled pool's first part) to none.
    float c[3], e[3], d2 = 0.0f;
    DrawWorldBox(c, e);
    if (m_frameEyeValid)
        for (int j = 0; j < 3; ++j) {
            float d = std::max(std::fabs(m_frameEye[j] - c[j]) - e[j], 0.0f);
            d2 += d * d;
        }
    const float dist = std::sqrt(d2), reach = std::max(m_leaf.distance, 1.0f);
    const float keep = std::clamp((reach - dist) / (0.5f * reach), 0.0f, 1.0f);
    m_drawLeaves = &set;
    m_drawLeafKey = key;
    m_drawLeafCount = uint32_t(float(set.count) * keep * keep);
    // Falling leaves: a few slots a tree (one a shrub), each now and then a leaf drifting down (ffp.vert FallingLeaf).
    if (m_drawLeafCount && m_leaf.fall > 0.0f && !set.art) {   // (our own canopy: a whole sprig would fall - later)
        m_drawLeafFall = std::min<uint32_t>(uint32_t(std::lround((set.kind == Canopy::Shrub ? 1.0f : 5.0f) * m_leaf.fall * keep)), 60u);
        m_drawLeafGround = LeafGround(set);
    }
}

// The crown: the canopy's box, its centre and half its largest extent (model space).
void Device::CrownOf(LeafSet& set) const
{
    const float* lo = m_drawMesh->boundsMin;
    const float* hi = m_drawMesh->boundsMax;
    for (int i = 0; i < 3; ++i)
        set.centre[i] = 0.5f * (lo[i] + hi[i]);
    set.radius = 0.5f * std::max(hi[0] - lo[0], std::max(hi[1] - lo[1], hi[2] - lo[2]));
}

// A solid static draw (opaque, depth-writing, lit, world-space): remembered a little while as a possible trunk, and
// matched to the canopy drawn just before it if it stands under that one.
void Device::NoteSolidDraw(uint32_t fvf, const void* vertices, uint32_t stride, uint32_t vertexCount)
{
    if (!m_leaf.on || !m_drawMesh || !m_drawMeshStatic || m_target != m_scene || IsTerrain(fvf) ||
        (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || !m_rs[d3d::RS_ZWRITEENABLE] || !m_rs[d3d::RS_LIGHTING] ||
        m_rs[d3d::RS_ALPHABLENDENABLE] || (m_textures[0] && !m_textures[0]->m_opaque))
        return;
    float c[3], e[3];
    DrawWorldBox(c, e);
    SolidBox& b = m_recentSolid[m_recentSolidNext++ % 16];
    for (int i = 0; i < 3; ++i) { b.lo[i] = c[i] - e[i]; b.hi[i] = c[i] + e[i]; }
    b.draw = m_frameDraw;
    // Its axis (a trunk's), once a mesh, in the world for our own canopies to start their branches on.
    b.axisPoints = 0;
    if (m_leaf.own) {
        const MeshInfo& info = *m_drawMesh;
        if (!info.trunkAxisState && vertices) {
            const auto& w = m_world.m;
            int k = 1;
            float best = -1.0f;
            for (int i = 0; i < 3; ++i) {
                const float len = std::sqrt(w[i][0] * w[i][0] + w[i][1] * w[i][1] + w[i][2] * w[i][2]);
                const float upness = len > 0.0f ? std::fabs(w[i][1]) / len : 0.0f;
                if (upness > best) { best = upness; k = i; }
            }
            TraceAxis(static_cast<const uint8_t*>(vertices), stride, vertexCount, k, info.trunkAxis);
            info.trunkAxisState = 1;
        }
        const auto& w = m_world.m;
        for (size_t i = 0; i + 2 < info.trunkAxis.size() && b.axisPoints < 10; i += 3, ++b.axisPoints) {
            const float* q = &info.trunkAxis[i];
            for (int j = 0; j < 3; ++j)
                b.axis[b.axisPoints * 3 + j] = q[0] * w[0][j] + q[1] * w[1][j] + q[2] * w[2][j] + w[3][j];
        }
    }
    // The own canopy drawn just before, if this stands under it: its trunk.
    if (m_recentCanopy.setKey && m_frameDraw - m_recentCanopy.ownDraw <= 4 &&
        TrunkUnder(b, m_recentCanopy.lo, m_recentCanopy.hi))
        GiveTrunk(m_recentCanopy.setKey, m_recentCanopy.world, b);
    const RecentCanopy& r = m_recentCanopy;
    if (r.instance && m_frameDraw - r.draw <= 4 && r.x >= b.lo[0] - 0.5f && r.x <= b.hi[0] + 0.5f &&
        r.z >= b.lo[2] - 0.5f && r.z <= b.hi[2] + 0.5f && b.lo[1] < r.bottom)
        m_leafGround[r.instance] = b.lo[1] + 0.05f;
}

// Whether a solid draw's box stands under a crown's (world): overlapping it across, starting below it, with an axis.
bool Device::TrunkUnder(const SolidBox& b, const float lo[3], const float hi[3]) const
{
    return b.axisPoints >= 2 && b.lo[0] <= hi[0] && b.hi[0] >= lo[0] && b.lo[2] <= hi[2] && b.hi[2] >= lo[2] &&
           b.lo[1] < lo[1];
}

// Our own canopy's trunk found: its axis into the canopy's model space (every instance of the tree shares it), and
// the set baked again with it if it was baked without.
void Device::GiveTrunk(uint64_t setKey, const d3d::Matrix& canopyWorld, const SolidBox& b)
{
    auto it = m_leafSets.find(setKey);
    if (it == m_leafSets.end() || !it->second.trunk.empty())
        return;
    d3d::Matrix inv;
    if (!InvertMatrix(canopyWorld, &inv))
        return;
    LeafSet& set = it->second;
    for (uint32_t i = 0; i < b.axisPoints; ++i) {
        const float* p = &b.axis[i * 3];
        for (int j = 0; j < 3; ++j)
            set.trunk.push_back(p[0] * inv.m[0][j] + p[1] * inv.m[1][j] + p[2] * inv.m[2][j] + inv.m[3][j]);
    }
    if (set.baked)
        set.baked = false;                       // (again, with it)
    static uint32_t logged = 0;
    if (logged < 20 && ++logged)
        Log("leaves: a tree's trunk found (%u axis points, from %.1f to %.1f up in the canopy's space)", b.axisPoints,
            double(set.trunk[1]), double(set.trunk[set.trunk.size() - 2]));
}

// The ground under the current canopy (see m_leafGround): the captured terrain, the trunk drawn next to it, what was
// found for this tree before, or just under the crown.
float Device::LeafGround(const LeafSet& set)
{
    float c[3], e[3];
    DrawWorldBox(c, e);
    const float x = c[0], z = c[2], bottom = c[1] - e[1];
    float y;
    uint32_t colour, light;
    if (m_grassOn && GroundAt(x, z, &y, &colour, &light) && y <= bottom + 0.5f)
        return y;
    uint64_t instance = m_drawMeshKey * 0x9E3779B97F4A7C15ull;
    instance ^= uint64_t(int64_t(std::floor(x * 4.0f))) * 0xC2B2AE3D27D4EB4Full;
    instance ^= uint64_t(int64_t(std::floor(z * 4.0f))) * 0x165667B19E3779F9ull;
    m_recentCanopy = {instance, x, z, bottom, m_frameDraw};
    float best = 1e30f;
    for (const SolidBox& b : m_recentSolid)
        if (b.draw && m_frameDraw - b.draw <= 4 && x >= b.lo[0] - 0.5f && x <= b.hi[0] + 0.5f && z >= b.lo[2] - 0.5f &&
            z <= b.hi[2] + 0.5f && b.lo[1] < bottom)
            best = std::min(best, b.lo[1] + 0.05f);
    if (best < 1e30f) {
        if (m_leafGround.size() > 8192)
            m_leafGround.clear();
        m_leafGround[instance] = best;
        return best;
    }
    auto it = m_leafGround.find(instance);
    if (it != m_leafGround.end())
        return it->second;
    return bottom - 0.5f * e[1];                 // no trunk found: a little under the crown
}

// A tree with its own leaf art (docs/canopy-plan.md phase 2): sprigs from the type's atlas in place of the game's cards,
// where the cards have leaves (the crown the artist made). Each a small branch: its twig's base on the card, reaching
// out of the crown and lying mostly along its surface (so a crown seen from outside shows the sprigs' faces), a little
// drooping; a random one of the atlas' 16. About a twelfth of the crown's height, x RVK_LeafSize; RVK_LeafDensity x
// enough to cover the cards' leafy part about twice over. (LeafRecord: half u across the sprig, half v towards its base
// - the texture's v grows down the cell, to the twig's base.)
void Device::BakeOwnCanopy(LeafSet& set, uint32_t primitive, const FvfLayout& layout, const void* vertices,
                           uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount, void* outLeaves,
                           void* outBranches)
{
    auto& leaves = *static_cast<std::vector<LeafRecord>*>(outLeaves);
    auto& branches = *static_cast<std::vector<LeafRecord>*>(outBranches);
    std::vector<V3> bases;                       // each sprig's twig base (the branches reach them)
    Texture* tex = m_textures[0];
    const auto& mask = tex->m_alphaMask;
    const uint32_t mw = tex->m_alphaW, mh = tex->m_alphaH;
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    const uint32_t stride = layout.stride, uvOffset = uint32_t(layout.offset[4]);
    auto pos = [&](uint32_t i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * stride, 12);
        return V3{p[0], p[1], p[2]};
    };
    auto uvAt = [&](uint32_t i, float out[2]) { std::memcpy(out, base + size_t(i) * stride + uvOffset, 8); };
    // The model's scale and its up (the world's y in model space: the world matrix's second column).
    const auto& w = m_world.m;
    const float scale = std::max(std::sqrt(w[0][0] * w[0][0] + w[0][1] * w[0][1] + w[0][2] * w[0][2]), 1e-6f);
    const V3 up = Normalize(V3{w[0][1], w[1][1], w[2][1]});
    const float* lo = m_drawMesh->boundsMin;
    const float* hi = m_drawMesh->boundsMax;
    const V3 centre{set.centre[0], set.centre[1], set.centre[2]};
    const V3 half{std::max(0.5f * (hi[0] - lo[0]), 0.05f), std::max(0.5f * (hi[1] - lo[1]), 0.05f),
                  std::max(0.5f * (hi[2] - lo[2]), 0.05f)};
    const float heightW = 2.0f * std::fabs(Dot(half, V3{std::fabs(up.x), std::fabs(up.y), std::fabs(up.z)})) * scale;
    const float sizeW = std::clamp(0.11f * heightW, 1.0f, 3.2f) * std::clamp(m_leaf.size, 0.25f, 3.5f);
    const float size = sizeW / scale;            // model units
    const float perArea = 4.5f * std::clamp(m_leaf.density, 0.1f, 4.0f) / (0.45f * size * size);
    // Random numbers of their own for each triangle, each sample and each cluster (from the mesh's content and where
    // they are), not one sequence: the canopy's texture arrives at low quality first and is baked again at full - one
    // sample landing otherwise shifted every number after it, and the whole crown changed (the user: turned off and on,
    // the branches came out different).
    const uint64_t seed = ContentSeed(vertices, stride, vertexCount, indices, indexCount) ^ 0xC0A0B7ull;
    double wantTotal = 0.0;
    const uint32_t count = indices ? indexCount : vertexCount;
    const uint32_t triangles = primitive == d3d::TriangleList ? count / 3 : (count >= 3 ? count - 2 : 0);
    auto unorm16 = [](float x) { return uint32_t(std::clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f); };
    for (uint32_t t = 0; t < triangles && leaves.size() < 4 * kMaxLeavesPerSet; ++t) {
        uint32_t k[3];
        if (primitive == d3d::TriangleList) { k[0] = 3 * t; k[1] = 3 * t + 1; k[2] = 3 * t + 2; }
        else if (primitive == d3d::TriangleStrip) { k[0] = t; k[1] = t + 1; k[2] = t + 2; }
        else { k[0] = 0; k[1] = t + 1; k[2] = t + 2; }
        uint32_t vi[3];
        for (int j = 0; j < 3; ++j) vi[j] = indices ? indices[k[j]] : k[j];
        if (vi[0] >= vertexCount || vi[1] >= vertexCount || vi[2] >= vertexCount)
            continue;
        V3 p0 = pos(vi[0]), p1 = pos(vi[1]), p2 = pos(vi[2]);
        float t0[2], t1[2], t2[2];
        uvAt(vi[0], t0);
        uvAt(vi[1], t1);
        uvAt(vi[2], t2);
        V3 e1 = p1 - p0, e2 = p2 - p0;
        const float area = 0.5f * Length(Cross(e1, e2));
        if (area < 1e-8f)
            continue;
        float want = area * perArea;
        wantTotal += want;
        Rng tr(seed ^ (uint64_t(t) + 1) * 0x9E3779B97F4A7C15ull);
        uint32_t tries = uint32_t(want) + (tr.Unit() < want - std::floor(want) ? 1u : 0u);
        tries = std::min<uint32_t>(tries, 8192);
        for (uint32_t c = 0; c < tries; ++c) {
            Rng rng(seed ^ ((uint64_t(t) << 24) ^ c) * 0xD6E8FEB86659FD93ull ^ 0x51ull);
            float r1 = rng.Unit(), r2 = rng.Unit();
            if (r1 + r2 > 1.0f) { r1 = 1.0f - r1; r2 = 1.0f - r2; }
            float u = t0[0] + (t1[0] - t0[0]) * r1 + (t2[0] - t0[0]) * r2;
            float v = t0[1] + (t1[1] - t0[1]) * r1 + (t2[1] - t0[1]) * r2;
            if (MaskAlpha(mask, mw, mh, u, v) < 0.5f)
                continue;
            V3 p = p0 + e1 * r1 + e2 * r2;
            // Out of the crown (an ellipsoid's normal there), a direction along its surface, the sprig between.
            V3 d = p - centre;
            V3 n = Normalize(V3{d.x / (half.x * half.x), d.y / (half.y * half.y), d.z / (half.z * half.z)});
            V3 r{rng.Unit() - 0.5f, rng.Unit() - 0.5f, rng.Unit() - 0.5f};
            V3 tangent = Normalize(r - n * Dot(r, n));
            float lean = 0.25f + 0.55f * rng.Unit();
            V3 reach = Normalize(tangent * std::cos(lean) + n * std::sin(lean) - up * (0.3f * rng.Unit()));
            V3 across = Cross(reach, n);
            if (Length(across) < 1e-4f)
                across = Cross(reach, up);
            across = Normalize(across);
            V3 hu = across * (0.5f * size), hv = reach * (-0.5f * size);   // (v: towards the twig's base)
            V3 mid = p + reach * (0.45f * size);
            const uint32_t cell = rng.Next() % 15u;   // (the 16th: a piece of branch)
            LeafRecord rec{};
            rec.centre[0] = mid.x;
            rec.centre[1] = mid.y;
            rec.centre[2] = mid.z;
            rec.halfUxy = Halves(hu.x, hu.y);
            rec.halfUzVx = Halves(hu.z, hv.x);
            rec.halfVyz = Halves(hv.y, hv.z);
            rec.uv = unorm16((float(cell % 4u) + 0.5f) * 0.25f) | unorm16((float(cell / 4u) + 0.5f) * 0.25f) << 16;
            const uint32_t uh = 2046;            // (0.125: the cell's half, a hair inside it)
            rec.uvHalfPhase = uh | uh << 12 | (rng.Next() >> 24) << 24;
            leaves.push_back(rec);
            bases.push_back(p);
        }
    }
    if (bases.empty())
        return;
    // Its branches (the game's cards had them painted in; without them the sprigs float): the sprigs' bases gathered
    // into clusters (k-means: a branch's end each), a limb from the top of the trunk (under the crown's middle) to each,
    // arching up a little, and from each cluster twigs to a few of its sprigs. Each piece two crossed cards of the
    // atlas' last cell (a stick, wide at its base); a limb as thick as the sprigs it carries.
    // (How many, and where they start: from the crown's shape - not the samples that happened to land on leaf.)
    const uint32_t clusters = std::clamp<uint32_t>(uint32_t(wantTotal / 90.0), 4u, 48u);
    std::vector<V3> mean(clusters);
    std::vector<uint32_t> of(bases.size(), 0), members(clusters, 0);
    for (uint32_t c = 0; c < clusters; ++c) {    // (the sprig nearest each of points spread over the crown)
        const float z = 1.0f - 2.0f * (float(c) + 0.5f) / float(clusters), r = std::sqrt(std::max(1.0f - z * z, 0.0f));
        const float phi = 2.3999632f * float(c);
        const V3 target = centre + V3{half.x * r * std::cos(phi), half.y * z, half.z * r * std::sin(phi)} * 0.7f;
        float best = 1e30f;
        for (const V3& b : bases) {
            V3 d = b - target;
            if (Dot(d, d) < best) { best = Dot(d, d); mean[c] = b; }
        }
    }
    for (int iteration = 0; iteration < 6; ++iteration) {
        std::vector<V3> sum(clusters, V3{0, 0, 0});
        std::fill(members.begin(), members.end(), 0u);
        for (size_t i = 0; i < bases.size(); ++i) {
            float best = 1e30f;
            for (uint32_t c = 0; c < clusters; ++c) {
                V3 d = bases[i] - mean[c];
                float d2 = Dot(d, d);
                if (d2 < best) { best = d2; of[i] = c; }
            }
            sum[of[i]] = sum[of[i]] + bases[i];
            ++members[of[i]];
        }
        for (uint32_t c = 0; c < clusters; ++c)
            if (members[c]) mean[c] = sum[c] * (1.0f / float(members[c]));
    }
    // The crown's root: under the middle of its sprigs, at their lowest.
    V3 meanAll{0, 0, 0};
    float lowest = 1e30f;
    for (const V3& b : bases) {
        meanAll = meanAll + b;
        lowest = std::min(lowest, Dot(b, up));
    }
    meanAll = meanAll * (1.0f / float(bases.size()));
    V3 root = meanAll + up * (lowest - Dot(meanAll, up) - 0.05f * Length(half));
    // Its trunk (GiveTrunk: the axis of the solid draw under it, canopy model space): the root where it reaches the
    // crown's lowest sprigs (or its top, short of them), and every branch may start on it.
    std::vector<V3> trunk;
    for (size_t i = 0; i + 2 < set.trunk.size(); i += 3)
        trunk.push_back(V3{set.trunk[i], set.trunk[i + 1], set.trunk[i + 2]});
    // (The closest point of a polyline to q, from its point `from` on.)
    auto closestOn = [](const std::vector<V3>& line, V3 q, size_t from, float* dist2) {
        V3 best = line.empty() ? q : line[std::min(from, line.size() - 1)];
        float bd = 1e30f;
        for (size_t i = from; i + 1 < line.size(); ++i) {
            V3 a = line[i], ab = line[i + 1] - a;
            const float t = std::clamp(Dot(q - a, ab) / std::max(Dot(ab, ab), 1e-12f), 0.0f, 1.0f);
            V3 p = a + ab * t, d = q - p;
            if (Dot(d, d) < bd) { bd = Dot(d, d); best = p; }
        }
        if (line.size() == 1) { V3 d = q - best; bd = Dot(d, d); }
        if (dist2) *dist2 = bd;
        return best;
    };
    if (trunk.size() >= 2) {
        root = trunk.back();
        for (size_t i = 0; i + 1 < trunk.size(); ++i) {
            const float h0 = Dot(trunk[i], up), h1 = Dot(trunk[i + 1], up);
            if (h0 <= lowest && h1 >= lowest) {
                root = trunk[i] + (trunk[i + 1] - trunk[i]) * ((lowest - h0) / std::max(h1 - h0, 1e-6f));
                break;
            }
        }
    }
    // A piece of branch from `from` to `to`, `thick` wide (model units) all along (the atlas' last cell: an even
    // stick), a little longer at both ends so pieces in a row overlap (no seam at the joint).
    auto stick = [&](V3 from, V3 to, float thick) {
        V3 axis = to - from;
        const float len = Length(axis);
        if (len < 1e-4f)
            return;
        V3 a = axis * (1.0f / len);
        from = from - a * (0.4f * thick);
        to = to + a * (0.4f * thick);
        axis = to - from;
        V3 side = Cross(a, std::fabs(Dot(a, up)) < 0.9f ? up : V3{1, 0, 0});
        side = Normalize(side);
        V3 side2 = Normalize(Cross(a, side));
        const float halfU = 0.5f * thick / 0.30f;   // (the stick fills ~0.30 of the cell's width)
        V3 mid = (from + to) * 0.5f, hv = axis * -0.5f;   // (v: towards the base)
        for (V3 sd : {side, side2}) {
            V3 hu = sd * halfU;
            LeafRecord rec{};
            rec.centre[0] = mid.x;
            rec.centre[1] = mid.y;
            rec.centre[2] = mid.z;
            rec.halfUxy = Halves(hu.x, hu.y);
            rec.halfUzVx = Halves(hu.z, hv.x);
            rec.halfVyz = Halves(hv.y, hv.z);
            rec.uv = unorm16(0.875f) | unorm16(0.875f) << 16;
            const uint32_t uh = 2046;
            rec.uvHalfPhase = uh | uh << 12;
            branches.push_back(rec);
        }
    };
    // A curved branch (a quadratic curve from `from` through `bend` to `to`) in `pieces`, tapering evenly from w0 to w1;
    // its points kept (the branches off it start along it).
    struct Limb { std::vector<V3> pts; std::vector<float> widths; };
    auto curve = [&](V3 from, V3 bend, V3 to, float w0, float w1, int pieces, Limb* keep) {
        V3 prev = from;
        if (keep) { keep->pts.push_back(from); keep->widths.push_back(w0); }
        for (int k = 1; k <= pieces; ++k) {
            const float t = float(k) / float(pieces), u = 1.0f - t;
            V3 p = from * (u * u) + bend * (2.0f * u * t) + to * (t * t);
            const float w = w0 + (w1 - w0) * (float(k) - 0.5f) / float(pieces);
            stick(prev, p, w);
            if (keep) { keep->pts.push_back(p); keep->widths.push_back(w0 + (w1 - w0) * t); }
            prev = p;
        }
    };
    // A connected frame: nearest the root first; each further cluster hangs off the nearest one already in that is
    // nearer the root than itself (the first off the root) - starting along that one's limb where it passes closest
    // (not all from its end: a star), carrying on its way, curving up a little. As thick as all it carries.
    std::vector<uint32_t> order;
    for (uint32_t c = 0; c < clusters; ++c)
        if (members[c]) order.push_back(c);
    auto toRoot = [&](uint32_t c) { V3 d = mean[c] - root; return Dot(d, d); };
    std::sort(order.begin(), order.end(), [&](uint32_t x, uint32_t y) { return toRoot(x) < toRoot(y); });
    std::vector<int> parent(clusters, -1);
    for (size_t k = 1; k < order.size(); ++k) {
        float best = 1e30f;
        for (size_t j = 0; j < k; ++j) {
            V3 d = mean[order[k]] - mean[order[j]];
            const float d2 = Dot(d, d);
            if (d2 < best) { best = d2; parent[order[k]] = int(order[j]); }
        }
    }
    std::vector<float> carried(clusters, 0.0f);
    for (size_t k = order.size(); k-- > 0;) {
        const uint32_t c = order[k];
        carried[c] += float(members[c]);
        if (parent[c] >= 0) carried[parent[c]] += carried[c];
    }
    const float limbW = std::clamp(0.08f * sizeW, 0.1f, 0.45f) / scale;   // a cluster's own branch (model units)
    std::vector<Limb> limbs(clusters);
    for (uint32_t c : order) {
        float thick = limbW * std::min(std::sqrt(carried[c] / 35.0f), 4.0f);
        V3 from = root, onward = up;
        float viaParent = 1e30f;                 // (the attachment on its parent's limb, how far)
        V3 parentAt{};
        float parentW = 0.0f;
        V3 parentOnward = up;
        if (parent[c] >= 0) {                    // (the closest point of the parent's limb past its first third)
            const Limb& pl = limbs[parent[c]];
            float best = 1e30f;
            size_t at = pl.pts.size() - 1;
            for (size_t i = pl.pts.size() / 3; i < pl.pts.size(); ++i) {
                V3 d = pl.pts[i] - mean[c];
                if (Dot(d, d) < best) { best = Dot(d, d); at = i; }
            }
            parentAt = pl.pts[at];
            parentOnward = Normalize(pl.pts[at] - pl.pts[at > 0 ? at - 1 : 0] + up * 1e-3f);
            parentW = pl.widths[at];
            viaParent = best;
        }
        // On the trunk where it passes closest, if that is nearer than its parent's limb (or it has none).
        float viaTrunk = 1e30f;
        V3 trunkAt = trunk.size() >= 2 ? closestOn(trunk, mean[c], 0, &viaTrunk) : root;
        if (trunk.size() >= 2 && (parent[c] < 0 || viaTrunk < viaParent)) {
            from = trunkAt;
            onward = Normalize(mean[c] - trunkAt + up * (0.3f * Length(mean[c] - trunkAt)));
        } else if (parent[c] >= 0) {
            from = parentAt;
            onward = parentOnward;
            thick = std::min(thick, 0.85f * parentW);
        }
        V3 d = mean[c] - from;
        const float len = Length(d);
        if (len < 1e-4f)
            continue;
        V3 bend = from + Normalize(d * (1.0f / len) + onward * 0.6f) * (0.45f * len) + up * (0.08f * len);
        const int pieces = std::clamp(int(len / (2.0f * thick + 1e-4f)), 2, 5);
        curve(from, bend, mean[c], thick, thick * 0.55f, pieces, &limbs[c]);
        // Twigs to a few of its sprigs: off the limb's last part, thin and even, stopping short of the sprig's own.
        const Limb& l = limbs[c];
        Rng rng(seed ^ (uint64_t(c) + 1) * 0x2545F4914F6CDD1Dull);
        uint32_t twigs = 0;
        for (size_t i = 0; i < bases.size() && twigs < 4; ++i)
            if (of[i] == c && rng.Unit() < 4.0f / float(members[c] + 1)) {
                const size_t at = l.pts.size() - 1 - (rng.Next() % std::min<size_t>(l.pts.size(), 2));
                V3 start = l.pts[at], end = start + (bases[i] - start) * 0.85f;
                V3 mid = (start + end) * 0.5f + up * (0.06f * Length(end - start));
                curve(start, mid, end, limbW * 0.3f, limbW * 0.22f, 2, nullptr);
                ++twigs;
            }
    }
}

// Bakes a canopy's leaves (see the top of the file) into the pool.
bool Device::BakeLeaves(LeafSet& set, uint32_t primitive, const FvfLayout& layout, const void* vertices,
                        uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    const double since = ProfileCpu();
    Texture* tex = m_textures[0];
    FreeLeaves(set);
    set.baked = true;
    set.texVersion = tex->m_alphaVersion;
    set.settingsVersion = m_leafVersion;
    const auto& mask = tex->m_alphaMask;
    const uint32_t mw = tex->m_alphaW, mh = tex->m_alphaH;
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    const uint32_t stride = layout.stride, uvOffset = uint32_t(layout.offset[4]);
    auto pos = [&](uint32_t i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * stride, 12);
        return V3{p[0], p[1], p[2]};
    };
    auto uvAt = [&](uint32_t i, float out[2]) { std::memcpy(out, base + size_t(i) * stride + uvOffset, 8); };
    CrownOf(set);
    const V3 centre{set.centre[0], set.centre[1], set.centre[2]};

    const float win = 0.0625f * std::clamp(m_leaf.size, 0.25f, 3.5f);   // a sprig's half size in the texture
    const float perArea = 2.2f * std::clamp(m_leaf.density, 0.1f, 4.0f) / (4.0f * win * win);
    std::vector<LeafRecord> leaves;
    Rng rng(ContentSeed(vertices, layout.stride, vertexCount, indices, indexCount) ^ 0x5EEDull);
    const uint32_t count = indices ? indexCount : vertexCount;
    const uint32_t triangles = primitive == d3d::TriangleList ? count / 3 : (count >= 3 ? count - 2 : 0);
    std::vector<LeafRecord> branches;            // (our own canopy's: first, never thinned - BakeOwnCanopy)
    if (set.art)
        BakeOwnCanopy(set, primitive, layout, vertices, vertexCount, indices, indexCount, &leaves, &branches);
    for (uint32_t t = 0; t < (set.art ? 0u : triangles); ++t) {
        uint32_t k[3];
        if (primitive == d3d::TriangleList) { k[0] = 3 * t; k[1] = 3 * t + 1; k[2] = 3 * t + 2; }
        else if (primitive == d3d::TriangleStrip) { k[0] = t; k[1] = t + 1; k[2] = t + 2; }
        else { k[0] = 0; k[1] = t + 1; k[2] = t + 2; }
        uint32_t vi[3];
        for (int j = 0; j < 3; ++j) vi[j] = indices ? indices[k[j]] : k[j];
        if (vi[0] >= vertexCount || vi[1] >= vertexCount || vi[2] >= vertexCount)
            continue;
        V3 p0 = pos(vi[0]), p1 = pos(vi[1]), p2 = pos(vi[2]);
        float t0[2], t1[2], t2[2];
        uvAt(vi[0], t0);
        uvAt(vi[1], t1);
        uvAt(vi[2], t2);
        V3 e1 = p1 - p0, e2 = p2 - p0;
        float du1 = t1[0] - t0[0], dv1 = t1[1] - t0[1], du2 = t2[0] - t0[0], dv2 = t2[1] - t0[1];
        float det = du1 * dv2 - du2 * dv1;
        float uvArea = 0.5f * std::fabs(det);
        if (uvArea < 1e-6f || Length(Cross(e1, e2)) < 1e-8f)
            continue;
        // The card's own directions of u and v (model units per unit of the texture): a sprig cut from the texture
        // keeps its painted proportions.
        V3 du = (e1 * dv2 - e2 * dv1) * (1.0f / det), dv = (e2 * du1 - e1 * du2) * (1.0f / det);
        V3 n = Normalize(Cross(e1, e2));
        float want = uvArea * perArea;
        uint32_t tries = uint32_t(want) + (rng.Unit() < want - std::floor(want) ? 1u : 0u);
        tries = std::min<uint32_t>(tries, 4096);
        for (uint32_t c = 0; c < tries; ++c) {
            float r1 = rng.Unit(), r2 = rng.Unit();
            if (r1 + r2 > 1.0f) { r1 = 1.0f - r1; r2 = 1.0f - r2; }
            float u = t0[0] + du1 * r1 + du2 * r2, v = t0[1] + dv1 * r1 + dv2 * r2;
            // Leaf there, and leafy around it (not a lone twig at the cluster's edge).
            if (MaskAlpha(mask, mw, mh, u, v) < 0.5f)
                continue;
            float around = 0.0f;
            const float o = 0.6f * win;
            around += MaskAlpha(mask, mw, mh, u - o, v) + MaskAlpha(mask, mw, mh, u + o, v);
            around += MaskAlpha(mask, mw, mh, u, v - o) + MaskAlpha(mask, mw, mh, u, v + o);
            if (around < 4.0f * 0.35f)
                continue;
            V3 p = p0 + e1 * r1 + e2 * r2;
            V3 hu = du * win, hv = dv * win;
            // Its own way: turned about the card's normal, then tilted off it (up to ~40 degrees).
            float turn = (rng.Unit() - 0.5f) * 6.2831853f;
            hu = Rotate(hu, n, turn);
            hv = Rotate(hv, n, turn);
            V3 tiltAxis = Normalize(Rotate(Normalize(du), n, rng.Unit() * 6.2831853f));
            float tilt = (rng.Unit() - 0.5f) * 1.4f;
            hu = Rotate(hu, tiltAxis, tilt);
            hv = Rotate(hv, tiltAxis, tilt);
            // Lifted off the card (either side), and a little out of the crown: depth.
            float size = Length(hv);
            p = p + n * ((rng.Unit() - 0.5f) * 1.2f * size);
            V3 out = p - centre;
            p = p + Normalize(out) * (rng.Unit() * 0.5f * size);
            LeafRecord r{};
            r.centre[0] = p.x;
            r.centre[1] = p.y;
            r.centre[2] = p.z;
            r.halfUxy = Halves(hu.x, hu.y);
            r.halfUzVx = Halves(hu.z, hv.x);
            r.halfVyz = Halves(hv.y, hv.z);
            auto unorm16 = [](float x) { return uint32_t(std::clamp(x, 0.0f, 1.0f) * 65535.0f + 0.5f); };
            r.uv = unorm16(u - std::floor(u)) | unorm16(v - std::floor(v)) << 16;
            uint32_t uh = uint32_t(std::min(win / 0.25f, 1.0f) * 4095.0f + 0.5f);
            r.uvHalfPhase = uh | uh << 12 | (rng.Next() >> 24) << 24;
            leaves.push_back(r);
        }
    }
    // Shuffled: any first part of the set is an even thinning (the distance level of detail draws a first part).
    for (size_t i = leaves.size(); i > 1; --i)
        std::swap(leaves[i - 1], leaves[rng.Next() % i]);
    if (leaves.size() + branches.size() > kMaxLeavesPerSet)
        leaves.resize(kMaxLeavesPerSet - std::min<size_t>(branches.size(), kMaxLeavesPerSet));
    leaves.insert(leaves.begin(), branches.begin(), branches.end());
    const uint32_t n = uint32_t(leaves.size());
    if (n) {
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = n;                             // in leaves
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (!m_leafPoolBlock || vmaVirtualAllocate(m_leafPoolBlock, &ai, &a, &offset) != VK_SUCCESS) {
            if (!GrowLeafPool(m_leafPoolLeaves + n) || vmaVirtualAllocate(m_leafPoolBlock, &ai, &a, &offset) != VK_SUCCESS)
                return false;
        }
        const VkDeviceSize bytes = VkDeviceSize(n) * sizeof(LeafRecord);
        EnsureRingSpace(bytes + 64);
        void* cpu;
        const VkDeviceSize staging = Allocate(bytes, 16, &cpu);
        std::memcpy(cpu, leaves.data(), bytes);
        VkBufferCopy region{staging, offset * sizeof(LeafRecord), bytes};
        vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, m_leafPool, 1, &region);
        m_skinUploadsPending = true;             // the upload buffer's closing barrier covers it
        uint64_t handle = 0;
        static_assert(sizeof(a) <= sizeof(handle), "virtual allocation handle");
        std::memcpy(&handle, &a, sizeof(a));
        set.alloc = handle;
        set.first = uint32_t(offset);
        set.count = n;
    }
    ++m_leafBakes;
    m_leafBakedLeaves += n;
    m_leafBakeMs += ProfileCpu() - since;
    return true;
}

// A set's leaves leave the pool once no frame in flight can still draw them.
void Device::FreeLeaves(LeafSet& set)
{
    if (set.alloc)
        m_leafTrash.push_back({set.alloc, VK_NULL_HANDLE, nullptr, m_frameNumber});
    set.alloc = 0;
    set.first = set.count = 0;
}

// The leaf pool, at least `minLeaves` big: a new, bigger buffer that the baked sets are copied into (the old one is
// freed once no frame in flight reads it).
bool Device::GrowLeafPool(uint32_t minLeaves)
{
    uint32_t size = std::max<uint32_t>(m_leafPoolLeaves * 2, 1u << 16);   // 2 MB to start
    while (size < minLeaves)
        size *= 2;
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = VkDeviceSize(size) * sizeof(LeafRecord);
    bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation allocation = nullptr;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &buffer, &allocation, nullptr), "leaf pool", &err))
        return false;
    VmaVirtualBlockCreateInfo vb{};
    vb.size = size;                              // counted in leaves
    VmaVirtualBlock block = nullptr;
    if (vmaCreateVirtualBlock(&vb, &block) != VK_SUCCESS) {
        vmaDestroyBuffer(m_allocator, buffer, allocation);
        return false;
    }
    std::vector<VkBufferCopy> regions;
    for (auto& [key, set] : m_leafSets) {
        if (!set.alloc)
            continue;
        VmaVirtualAllocationCreateInfo ai{};
        ai.size = set.count;
        VmaVirtualAllocation a{};
        VkDeviceSize offset = 0;
        if (vmaVirtualAllocate(block, &ai, &a, &offset) != VK_SUCCESS) {   // can't happen: the new pool is bigger
            set.alloc = 0;
            set.count = 0;
            set.baked = false;
            continue;
        }
        regions.push_back({VkDeviceSize(set.first) * sizeof(LeafRecord), offset * sizeof(LeafRecord),
                           VkDeviceSize(set.count) * sizeof(LeafRecord)});
        uint64_t handle = 0;
        std::memcpy(&handle, &a, sizeof(a));
        set.alloc = handle;
        set.first = uint32_t(offset);
    }
    if (m_leafPool) {
        VkCommandBuffer cmd = UploadCommands();
        if (!regions.empty()) {
            VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
            mb.srcStageMask = mb.dstStageMask = VK_PIPELINE_STAGE_2_COPY_BIT;
            mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            mb.dstAccessMask = VK_ACCESS_2_TRANSFER_READ_BIT;
            VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.memoryBarrierCount = 1;
            dep.pMemoryBarriers = &mb;
            vkCmdPipelineBarrier2(cmd, &dep);
            vkCmdCopyBuffer(cmd, m_leafPool, buffer, uint32_t(regions.size()), regions.data());
            m_skinUploadsPending = true;
        }
        m_leafTrash.push_back({0, m_leafPool, m_leafPoolAllocation, m_frameNumber});
    }
    for (auto& t : m_leafTrash)                  // the old block's ranges are gone with it
        t.alloc = 0;
    if (m_leafPoolBlock) {
        vmaClearVirtualBlock(m_leafPoolBlock);
        vmaDestroyVirtualBlock(m_leafPoolBlock);
    }
    m_leafPool = buffer;
    m_leafPoolAllocation = allocation;
    m_leafPoolBlock = block;
    m_leafPoolLeaves = size;
    Log("leaves: pool %u leaves (%.1f MB)", size, double(bi.size) / (1024.0 * 1024.0));
    return true;
}

// Once a frame: the trash, the sets no longer drawn, a settings change (every set baked again as it is next drawn),
// and with the leaves off everything they hold.
void Device::UpdateLeaves()
{
    m_leafBakesThisFrame = 0;
    for (auto it = m_leafTrash.begin(); it != m_leafTrash.end();) {
        if (it->frame + kFramesInFlight + 1 <= m_frameNumber) {
            if (it->alloc && m_leafPoolBlock) {
                VmaVirtualAllocation a{};
                std::memcpy(&a, &it->alloc, sizeof(a));
                vmaVirtualFree(m_leafPoolBlock, a);
            }
            if (it->buffer)
                vmaDestroyBuffer(m_allocator, it->buffer, it->allocation);
            it = m_leafTrash.erase(it);
        } else {
            ++it;
        }
    }
    if (m_leafDirty) {
        m_leafDirty = false;
        ++m_leafVersion;
    }
    if (!m_leaf.on) {
        if (!m_leafSets.empty()) {
            for (auto& [key, set] : m_leafSets)
                FreeLeaves(set);
            m_leafSets.clear();
        }
        return;
    }
    if ((m_frameNumber & 255) == 0)
        for (auto it = m_leafSets.begin(); it != m_leafSets.end();) {
            if (it->second.lastFrame + kLeafSetIdleFrames < m_frameNumber) {
                FreeLeaves(it->second);
                it = m_leafSets.erase(it);
            } else {
                ++it;
            }
        }
}

// The current canopy's leaves, drawn before its own draw (Draw: its state, descriptors and geometry are already set up
// for it): through leaf.vert, with its record, every face (a leaf is seen from both sides), then the canopy's pipeline,
// culling and per-draw bindings back.
void Device::DrawLeaves(VkCommandBuffer cmd, uint32_t recordIndex, uint32_t primitive, uint32_t fvf, uint32_t stride,
                        VkDeviceSize frameLightsOffset, VkBuffer prevBuffer, VkDeviceSize prevOffset,
                        VkDeviceSize prevBytes, VkBuffer smoothBuffer, VkDeviceSize smoothOffset,
                        VkDeviceSize smoothBytes)
{
    const LeafSet& set = *m_drawLeaves;
    const bool hdr = m_target->m_format == Format::RGBA16F;
    StateCache& c = m_cache;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_leafPipelines[hdr ? 1 : 0]);
    vkCmdSetCullMode(cmd, VK_CULL_MODE_NONE);
    vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
    PushDrawSet(cmd, frameLightsOffset, prevBuffer, prevOffset, prevBytes, m_leafPool, 0,
                VkDeviceSize(m_leafPoolLeaves) * sizeof(LeafRecord));
    vkCmdDraw(cmd, (m_drawLeafCount + m_drawLeafFall) * 6u, 1, set.first * 6u, recordIndex);
    ++m_leafDraws;
    m_leafDrawn += m_drawLeafCount;
    // The canopy's own state again.
    c.topologyClass = ~0u;
    c.topology = ~0u;
    c.cull = ~0u;
    ApplyDynamicState(primitive, fvf, stride);
    PushDrawSet(cmd, frameLightsOffset, prevBuffer, prevOffset, prevBytes, smoothBuffer, smoothOffset, smoothBytes);
}

// The current sun cascade's canopies' leaves (RenderShadowMap, after the cascade's casters, with its rendering open and
// its light matrix pushed): each visible caster with a leaf set, with its record (drawn in already: the crown and the
// wind), through leaf_shadow.vert - as many leaves as the scene draws at its distance. Rebinds what it changes; the
// cascade's rendering ends right after.
void Device::DrawLeafShadows(VkCommandBuffer cmd)
{
    if (!m_leaf.on || !m_leaf.shadows || !m_leafShadowPipeline || !m_leafPool)
        return;
    bool bound = false;
    const float reach = std::max(m_leaf.distance, 1.0f);
    for (uint32_t i : m_cascadeVisible) {
        const ShadowItem& item = m_shadowItems[i];
        if (!item.leafKey || !item.texture || item.record >= kNoShadowRecord)
            continue;
        auto it = m_leafSets.find(item.leafKey);
        if (it == m_leafSets.end() || !it->second.count)
            continue;
        const LeafSet& set = it->second;
        float d2 = 0.0f;
        for (int j = 0; j < 3; ++j) {
            float d = std::max(std::max(item.boundsMin[j] - m_frameEye[j], m_frameEye[j] - item.boundsMax[j]), 0.0f);
            d2 += d * d;
        }
        const float keep = std::clamp((reach - std::sqrt(d2)) / (0.5f * reach), 0.0f, 1.0f);
        const uint32_t count = uint32_t(float(set.count) * keep * keep);
        if (!count)
            continue;
        if (!bound) {
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_leafShadowPipeline);
            vkCmdSetPrimitiveTopology(cmd, VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST);
            vkCmdSetVertexInputEXT(cmd, 0, nullptr, 0, nullptr);
            VkDescriptorBufferInfo pool{m_leafPool, 0, VkDeviceSize(m_leafPoolLeaves) * sizeof(LeafRecord)};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 2;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w.pBufferInfo = &pool;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
            bound = true;
        }
        Texture* drawn = set.art ? set.art : item.texture;   // (our own canopy: its atlas)
        VkDescriptorImageInfo image{SamplerFor(0), drawn->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w.pImageInfo = &image;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_shadowPipelineLayout, 0, 1, &w);
        vkCmdDraw(cmd, count * 6u, 1, set.first * 6u, item.record);
        m_leafShadowLeaves += count;
    }
}

void Device::DestroyLeafResources()
{
    for (auto& [key, set] : m_leafSets)
        set.alloc = 0;
    m_leafSets.clear();
    for (auto& t : m_leafTrash)
        if (t.buffer)
            vmaDestroyBuffer(m_allocator, t.buffer, t.allocation);
    m_leafTrash.clear();
    if (m_leafPoolBlock) {
        vmaClearVirtualBlock(m_leafPoolBlock);
        vmaDestroyVirtualBlock(m_leafPoolBlock);
        m_leafPoolBlock = nullptr;
    }
    if (m_leafPool)
        vmaDestroyBuffer(m_allocator, m_leafPool, m_leafPoolAllocation);
    m_leafPool = VK_NULL_HANDLE;
    m_leafPoolAllocation = nullptr;
    m_leafPoolLeaves = 0;
    for (VkPipeline& p : m_leafPipelines) {
        if (p) vkDestroyPipeline(m_device, p, nullptr);
        p = VK_NULL_HANDLE;
    }
}

}  // namespace rvk
