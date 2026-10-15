// The sand's shelter (docs/sand-shelter-plan.md): what stands round the camera, and what it shelters from the storm's
// wind - so no sand blows in the lee of a hill, a wall or a rock, less behind a tree, and the sand goes round trunks.
//
// The map: kMapN x kMapN cells of kMapCell world units round the eye (moved when the eye is a quarter of it from its
// middle), per cell
// - the ground: the grass's terrain capture (GroundAt); without it the terrain grid of terrain_sun.cpp; else unknown;
// - solid: the top of what stands there - every placed object's (statels.cpp) solid pieces rasterised from above
//   (a triangle marks every cell it touches: a wall seen from above is a line);
// - crown: the span (lowest, highest) of see-through pieces over it (canopies, fences: the wind goes through, slowed).
// Built in slices (a time budget a frame) into the next map, swapped in when done; made again every few seconds as the
// ground comes in.
//
// Shelter, per cell: the height under which the wind is held off - a solid obstacle upwind shelters out to ~7 times its
// height behind it (Hs falls a unit for every kSolidLee), a see-through one to ~4 times and half as much (Hp). One
// sweep along the wind: Hs(cell) = max(its own top, Hs two cells upwind - the fall over that); upwind of the map, the
// same sweep over the terrain grid (16 units a cell: the hills far off). Made again when the wind turns a degree or
// the map changes.
//
// Used by the sand volume (smoke.cpp MakeSandGround: each column's top, its sand, the shelter heights; smoke_sim.comp
// SandShelter) and the haze near the camera (SandHaze).
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {
constexpr int kMapN = 192;                       // cells a side
constexpr float kMapCell = 0.5f;                 // world units
constexpr float kSolidLee = 7.0f;                // a solid obstacle's lee: its height x this
constexpr float kCrownLee = 4.0f;                // a see-through one's
constexpr float kUnknown = -1e9f;
constexpr double kBudgetMs = 0.8;                // building a frame
constexpr int kCoarseN = 256;                    // terrain_sun.cpp's grid
constexpr float kCoarseCell = 16.0f;

double NowMs()
{
    using namespace std::chrono;
    return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
}

// Whether a 2D triangle (its points a, b, c) touches the box [x0, x1] x [z0, z1] (separating axes: the box's two, the
// triangle's edge normals - a degenerate triangle, a wall seen from above, as the segment it is).
bool TriangleTouchesBox(const float a[2], const float b[2], const float c[2], float x0, float z0, float x1, float z1)
{
    const float* p[3] = {a, b, c};
    for (int e = 0; e < 3; ++e) {
        const float* u = p[e];
        const float* v = p[(e + 1) % 3];
        const float nx = -(v[1] - u[1]), nz = v[0] - u[0];
        if (nx == 0.0f && nz == 0.0f) continue;
        float tmin = 1e30f, tmax = -1e30f;
        for (int k = 0; k < 3; ++k) {
            const float t = p[k][0] * nx + p[k][1] * nz;
            tmin = std::min(tmin, t);
            tmax = std::max(tmax, t);
        }
        float bmin = 1e30f, bmax = -1e30f;
        for (int k = 0; k < 4; ++k) {
            const float t = (k & 1 ? x1 : x0) * nx + (k & 2 ? z1 : z0) * nz;
            bmin = std::min(bmin, t);
            bmax = std::max(bmax, t);
        }
        if (bmax < tmin || bmin > tmax) return false;
    }
    return true;
}

// The shelter sweep over a grid (n x n, `cell` world units, the wind `w` towards x, z): out = max(top, out two cells
// upwind - the fall). Along the wind's larger axis, the cells two upwind are always done before (their bilinear corners
// lie a whole cell or more back along it). `edge`: the value upwind of the grid at a world x, z (or kUnknown).
template <typename Top, typename Edge>
void ShelterSweep(int n, float cell, const float w[2], float fallPerCell, Top top, Edge edge, std::vector<float>& out)
{
    out.assign(size_t(n) * n, kUnknown);
    const bool alongX = std::fabs(w[0]) >= std::fabs(w[1]);
    const int dirMajor = (alongX ? w[0] : w[1]) >= 0.0f ? 1 : -1;
    const float bx = -2.0f * w[0], bz = -2.0f * w[1];   // (two cells upwind)
    const float fall = 2.0f * fallPerCell;
    auto at = [&](int x, int z) { return out[size_t(z) * n + x]; };
    for (int i = 0; i < n; ++i) {
        const int major = dirMajor > 0 ? i : n - 1 - i;
        for (int minor = 0; minor < n; ++minor) {
            const int x = alongX ? major : minor, z = alongX ? minor : major;
            const float px = float(x) + bx, pz = float(z) + bz;
            float up;
            if (px < 0.0f || pz < 0.0f || px > float(n - 1) || pz > float(n - 1)) {
                up = edge((px + 0.5f) * cell, (pz + 0.5f) * cell);
            } else {
                const int x0 = int(px), z0 = int(pz), x1 = std::min(x0 + 1, n - 1), z1 = std::min(z0 + 1, n - 1);
                const float fx = px - float(x0), fz = pz - float(z0);
                const float v00 = at(x0, z0), v10 = at(x1, z0), v01 = at(x0, z1), v11 = at(x1, z1);
                if (v00 <= kUnknown * 0.5f || v10 <= kUnknown * 0.5f || v01 <= kUnknown * 0.5f || v11 <= kUnknown * 0.5f)
                    up = std::max({v00, v10, v01, v11});
                else
                    up = (v00 * (1 - fx) + v10 * fx) * (1 - fz) + (v01 * (1 - fx) + v11 * fx) * fz;
            }
            out[size_t(z) * n + x] = std::max(top(x, z), up > kUnknown * 0.5f ? up - fall : kUnknown);
        }
    }
}

}  // namespace

// Once a frame while the sand blows (UpdateSandVolume): the map built (in slices), its shelter swept, the coarse
// terrain's.
void Device::UpdateSandMap()
{
    const double start = NowMs();
    // The terrain grid moved with the eye (terrain_sun.cpp keeps it; it fills from the terrain as drawn).
    if (!m_tsHeights.empty())
        RecentreTerrainSun();
    // A new map wanted: none yet, the eye a quarter of it from its middle, the placed objects changed, or it is old
    // (the ground comes in as the grass captures it).
    const int ex = int(std::floor(m_frameEye[0] / kMapCell)) - kMapN / 2;
    const int ez = int(std::floor(m_frameEye[2] / kMapCell)) - kMapN / 2;
    SandMapJob& job = m_sandMapJob;
    if (!job.active) {
        const bool moved = !m_sandMap.valid || std::abs(ex - m_sandMap.origin[0]) > kMapN / 4 ||
                           std::abs(ez - m_sandMap.origin[1]) > kMapN / 4;
        const bool stale = m_frameClock - m_sandMap.built > 4.0 || m_sandMap.statelGen != m_statelGeneration;
        if (moved || stale) {
            job = SandMapJob{};
            job.active = true;
            SandMap& m = m_sandMapNext;
            m.origin[0] = ex;
            m.origin[1] = ez;
            const size_t cells = size_t(kMapN) * kMapN;
            m.ground.assign(cells, kUnknown);
            m.solid.assign(cells, kUnknown);
            m.crownLo.assign(cells, 1e9f);
            m.crownHi.assign(cells, kUnknown);
            m.feed.assign(cells, 1.0f);
            m.statelGen = m_statelGeneration;
            const float x0 = float(ex) * kMapCell, z0 = float(ez) * kMapCell, size = kMapN * kMapCell;
            StatelItemsIn(x0, z0, x0 + size, z0 + size, job.items);
        }
    }
    if (job.active) {
        SandMap& m = m_sandMapNext;
        const float ox = float(m.origin[0]) * kMapCell, oz = float(m.origin[1]) * kMapCell;
        // The ground, a row at a time.
        while (job.row < uint32_t(kMapN) && NowMs() - start < kBudgetMs) {
            const int z = int(job.row++);
            for (int x = 0; x < kMapN; ++x) {
                const float wx = ox + (float(x) + 0.5f) * kMapCell, wz = oz + (float(z) + 0.5f) * kMapCell;
                float y;
                uint32_t colour = 0;
                const size_t c = size_t(z) * kMapN + x;
                if (GroundAt(wx, wz, &y, &colour, nullptr)) {
                    m.ground[c] = y;
                    const float r = float((colour >> 16) & 0xFF), g = float((colour >> 8) & 0xFF), b = float(colour & 0xFF);
                    if (g > r * 1.05f && g > b) m.feed[c] = 0.25f;   // (green: grass gives less sand)
                } else {
                    m.ground[c] = CoarseTerrainAt(wx, wz);
                }
            }
        }
        // The placed objects, an item at a time.
        while (job.row >= uint32_t(kMapN) && job.item < job.items.size() && NowMs() - start < kBudgetMs)
            RasterSandItem(m, job.items[job.item++]);
        if (job.row >= uint32_t(kMapN) && job.item >= job.items.size()) {
            m.valid = true;
            m.built = m_frameClock;
            std::swap(m_sandMap, m_sandMapNext);
            job.active = false;
            m_sandShelterWind[0] = m_sandShelterWind[1] = 0.0f;   // (swept again below)
            static uint32_t logged = 0;
            if (logged < 8 && ++logged) {
                size_t solid = 0, crown = 0, ground = 0;
                for (size_t c = 0; c < m_sandMap.ground.size(); ++c) {
                    ground += m_sandMap.ground[c] > kUnknown * 0.5f;
                    solid += m_sandMap.solid[c] > m_sandMap.ground[c] + 0.3f;
                    crown += m_sandMap.crownHi[c] > kUnknown * 0.5f;
                }
                Log("sand map: %d x %d cells of %.1f round (%.0f %.0f): ground known %.0f%%, solid %.1f%%, see-through "
                    "%.1f%%; %zu placed pieces", kMapN, kMapN, double(kMapCell), double(m_frameEye[0]),
                    double(m_frameEye[2]), 100.0 * double(ground) / double(m_sandMap.ground.size()),
                    100.0 * double(solid) / double(m_sandMap.ground.size()),
                    100.0 * double(crown) / double(m_sandMap.ground.size()), job.items.size());
            }
        }
    }
    // The coarse terrain's shelter (the hills upwind of the map), at most once a second.
    const float* w = m_windDir;
    auto turned = [&](const float a[2]) { return a[0] * w[0] + a[1] * w[1] < 0.99985f; };   // (a degree)
    if (!m_tsHeights.empty() && (turned(m_sandCoarseWind) || m_sandCoarseOrigin[0] != m_tsOrigin[0] ||
                                 m_sandCoarseOrigin[1] != m_tsOrigin[1] || m_tsDirty) &&
        m_frameClock - m_sandCoarseMade > 1.0) {
        ShelterSweep(kCoarseN, kCoarseCell, w, kCoarseCell / kSolidLee,
                     [&](int x, int z) { return m_tsHeights[size_t(z) * kCoarseN + x]; },
                     [](float, float) { return kUnknown; }, m_sandCoarseShelter);
        m_sandCoarseOrigin[0] = m_tsOrigin[0];
        m_sandCoarseOrigin[1] = m_tsOrigin[1];
        std::memcpy(m_sandCoarseWind, w, sizeof(m_sandCoarseWind));
        m_sandCoarseMade = m_frameClock;
        m_sandShelterWind[0] = m_sandShelterWind[1] = 0.0f;
    }
    // The map's shelter.
    if (m_sandMap.valid && turned(m_sandShelterWind)) {
        SandMap& m = m_sandMap;
        const float ox = float(m.origin[0]) * kMapCell, oz = float(m.origin[1]) * kMapCell;
        auto top = [&](int x, int z) {
            const size_t c = size_t(z) * kMapN + x;
            return std::max(m.ground[c], m.solid[c]);
        };
        auto edge = [&](float lx, float lz) {     // (upwind of the map: the coarse terrain's, a unit lower - its cells
            return CoarseShelterAt(ox + lx, oz + lz) - 1.0f;   // hold their highest point)
        };
        ShelterSweep(kMapN, kMapCell, w, kMapCell / kSolidLee, top, edge, m.shelterSolid);
        auto crown = [&](int x, int z) { return m.crownHi[size_t(z) * kMapN + x]; };
        ShelterSweep(kMapN, kMapCell, w, kMapCell / kCrownLee, crown, [](float, float) { return kUnknown; },
                     m.shelterCrown);
        std::memcpy(m_sandShelterWind, w, sizeof(m_sandShelterWind));
        ++m_sandShelterGen;
    }
    m_sandMapMs += NowMs() - start;
    if (m_frameNumber % 600 == 300) {
        Log("sand map: %.3f ms a frame (build, shelter: last 600 frames); eye's shelter %.2f", m_sandMapMs / 600.0,
            double(m_sandEyeShelter));
        m_sandMapMs = 0.0;
    }
}

// A placed piece rasterised into the map from above.
void Device::RasterSandItem(SandMap& m, uint32_t index)
{
    const StatelItem& it = m_statelItems[index];
    const StatelPiece& p = m_statelPieces[it.piece];
    const bool seeThrough = p.alphaRef >= 0.0f;
    const float ox = float(m.origin[0]) * kMapCell, oz = float(m.origin[1]) * kMapCell;
    if (uint64_t(p.firstVertex) + p.vertexCount > m_statelCpuPositions.size() / 3 ||
        uint64_t(p.firstIndex) + p.indexCount > m_statelCpuIndices.size())
        return;
    const auto& w = it.world.m;
    auto place = [&](uint16_t i, float out[3]) {
        const float* v = &m_statelCpuPositions[(size_t(p.firstVertex) + i) * 3];
        for (int k = 0; k < 3; ++k) out[k] = v[0] * w[0][k] + v[1] * w[1][k] + v[2] * w[2][k] + w[3][k];
    };
    for (uint32_t t = 0; t + 2 < p.indexCount; t += 3) {
        float a[3], b[3], c[3];
        const uint16_t* idx = &m_statelCpuIndices[p.firstIndex + t];
        if (idx[0] >= p.vertexCount || idx[1] >= p.vertexCount || idx[2] >= p.vertexCount)
            continue;
        place(idx[0], a);
        place(idx[1], b);
        place(idx[2], c);
        const float a2[2] = {a[0] - ox, a[2] - oz}, b2[2] = {b[0] - ox, b[2] - oz}, c2[2] = {c[0] - ox, c[2] - oz};
        const int x0 = std::max(int(std::floor(std::min({a2[0], b2[0], c2[0]}) / kMapCell)), 0);
        const int x1 = std::min(int(std::floor(std::max({a2[0], b2[0], c2[0]}) / kMapCell)), kMapN - 1);
        const int z0 = std::max(int(std::floor(std::min({a2[1], b2[1], c2[1]}) / kMapCell)), 0);
        const int z1 = std::min(int(std::floor(std::max({a2[1], b2[1], c2[1]}) / kMapCell)), kMapN - 1);
        if (x0 > x1 || z0 > z1)
            continue;
        const float hi = std::max({a[1], b[1], c[1]}), lo = std::min({a[1], b[1], c[1]});
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (!TriangleTouchesBox(a2, b2, c2, float(x) * kMapCell, float(z) * kMapCell, float(x + 1) * kMapCell,
                                        float(z + 1) * kMapCell))
                    continue;
                const size_t cell = size_t(z) * kMapN + x;
                if (seeThrough) {
                    m.crownHi[cell] = std::max(m.crownHi[cell], hi);
                    m.crownLo[cell] = std::min(m.crownLo[cell], lo);
                } else {
                    m.solid[cell] = std::max(m.solid[cell], hi);
                }
            }
    }
}

// The terrain grid's height at a world x, z (bilinear over its known cells; kUnknown outside or unknown).
float Device::CoarseTerrainAt(float x, float z) const
{
    if (m_tsHeights.empty())
        return kUnknown;
    const float fx = x / kCoarseCell - float(m_tsOrigin[0]) - 0.5f, fz = z / kCoarseCell - float(m_tsOrigin[1]) - 0.5f;
    const int x0 = int(std::floor(fx)), z0 = int(std::floor(fz));
    if (x0 < 0 || z0 < 0 || x0 + 1 >= kCoarseN || z0 + 1 >= kCoarseN)
        return kUnknown;
    const float tx = fx - float(x0), tz = fz - float(z0);
    float v[4], sum = 0.0f, weight = 0.0f;
    for (int k = 0; k < 4; ++k) {
        v[k] = m_tsHeights[size_t(z0 + (k >> 1)) * kCoarseN + x0 + (k & 1)];
        const float wgt = (k & 1 ? tx : 1 - tx) * (k >> 1 ? tz : 1 - tz);
        if (v[k] > kUnknown * 0.5f) { sum += v[k] * wgt; weight += wgt; }
    }
    return weight > 1e-4f ? sum / weight : kUnknown;
}

// The coarse terrain's shelter height at a world x, z (kUnknown where it has none).
float Device::CoarseShelterAt(float x, float z) const
{
    if (m_sandCoarseShelter.empty())
        return kUnknown;
    const int cx = int(std::floor(x / kCoarseCell)) - m_sandCoarseOrigin[0];
    const int cz = int(std::floor(z / kCoarseCell)) - m_sandCoarseOrigin[1];
    if (cx < 0 || cz < 0 || cx >= kCoarseN || cz >= kCoarseN)
        return kUnknown;
    return m_sandCoarseShelter[size_t(cz) * kCoarseN + cx];
}

// What the map says at a world x, z (false outside it or before it is made): the ground, the solid top (or the
// ground), the shelter heights (solid, see-through), how much sand the ground gives.
bool Device::SandMapAt(float x, float z, SandColumn* out) const
{
    const SandMap& m = m_sandMap;
    if (!m.valid || m.shelterSolid.empty())
        return false;
    const int cx = int(std::floor(x / kMapCell)) - m.origin[0], cz = int(std::floor(z / kMapCell)) - m.origin[1];
    if (cx < 0 || cz < 0 || cx >= kMapN || cz >= kMapN)
        return false;
    const size_t c = size_t(cz) * kMapN + cx;
    out->ground = m.ground[c];
    out->top = std::max(m.ground[c], m.solid[c]);
    out->shelterSolid = m.shelterSolid[c];
    out->shelterCrown = m.shelterCrown[c];
    out->feed = m.feed[c];
    return out->ground > kUnknown * 0.5f;
}

// The highest solid top over a world square (the sand volume's column: a trunk thinner than the column still in it).
float Device::SandMapSolidIn(float x0, float z0, float x1, float z1) const
{
    const SandMap& m = m_sandMap;
    float top = kUnknown;
    if (!m.valid)
        return top;
    const int cx0 = std::max(int(std::floor(x0 / kMapCell)) - m.origin[0], 0);
    const int cx1 = std::min(int(std::floor(x1 / kMapCell)) - m.origin[0], kMapN - 1);
    const int cz0 = std::max(int(std::floor(z0 / kMapCell)) - m.origin[1], 0);
    const int cz1 = std::min(int(std::floor(z1 / kMapCell)) - m.origin[1], kMapN - 1);
    for (int z = cz0; z <= cz1; ++z)
        for (int x = cx0; x <= cx1; ++x)
            top = std::max(top, m.solid[size_t(z) * kMapN + x]);
    return top;
}

// How sheltered a point is (0 open .. 1 held off): below a shelter height that stands over the cell's own top, by how
// far (a band of half the excess, 0.3..3 units); a see-through obstacle's half as much. (The same as smoke_sim.comp
// SandShelter.)
float Device::SandShelterAt(float x, float y, float z) const
{
    SandColumn col;
    if (!SandMapAt(x, z, &col))
        return 0.0f;
    auto held = [&](float shelter, float strength) {
        const float excess = shelter - col.top;
        if (excess < 0.3f) return 0.0f;
        const float band = std::clamp(0.5f * excess, 0.3f, 3.0f);
        const float k = std::clamp((excess - 0.3f) / 0.9f, 0.0f, 1.0f);
        return strength * std::clamp((shelter - y) / band, 0.0f, 1.0f) * k * k * (3.0f - 2.0f * k);
    };
    return std::max(held(col.shelterSolid, 1.0f), held(col.shelterCrown, 0.5f));
}

// The frame dump's picture of the map (<dump>-sand.bmp, two pixels a cell, north up): the ground grey by its height,
// solid red, see-through green, the shelter blue by its height over the cell's top (white: the eye).
void Device::DumpSandMap(const std::string& path) const
{
    const SandMap& m = m_sandMap;
    if (!m.valid || m.shelterSolid.empty())
        return;
    FILE* file = std::fopen(path.c_str(), "wb");
    if (!file)
        return;
    const int s = 2, wpx = kMapN * s;
    const uint32_t rowBytes = uint32_t(wpx) * 3, imageSize = rowBytes * uint32_t(wpx);
    uint8_t header[54] = {'B', 'M'};
    auto put32 = [&](int at, uint32_t v) { std::memcpy(header + at, &v, 4); };
    put32(2, 54 + imageSize);
    put32(10, 54);
    put32(14, 40);
    put32(18, uint32_t(wpx));
    put32(22, uint32_t(wpx));
    header[26] = 1;
    header[28] = 24;
    put32(34, imageSize);
    std::fwrite(header, 1, 54, file);
    float lo = 1e30f, hi = -1e30f;
    for (float g : m.ground)
        if (g > kUnknown * 0.5f) { lo = std::min(lo, g); hi = std::max(hi, g); }
    const int ex = int(std::floor(m_frameEye[0] / kMapCell)) - m.origin[0];
    const int ez = int(std::floor(m_frameEye[2] / kMapCell)) - m.origin[1];
    std::vector<uint8_t> row(rowBytes);
    for (int py = 0; py < wpx; ++py) {           // (BMP rows bottom-up: z grows up the picture)
        const int z = py / s;
        for (int px = 0; px < wpx; ++px) {
            const int x = px / s;
            const size_t c = size_t(z) * kMapN + x;
            const float g = m.ground[c];
            float r = 0.0f, gr = 0.0f, b = 0.0f;
            if (g > kUnknown * 0.5f) {
                const float v = hi > lo ? 0.25f + 0.5f * (g - lo) / (hi - lo) : 0.5f;
                r = gr = b = v;
            }
            const float top = std::max(g, m.solid[c]);
            if (m.solid[c] > g + 0.3f) { r = 0.9f; gr = 0.2f; b = 0.15f; }
            else if (m.crownHi[c] > kUnknown * 0.5f) { gr = std::max(gr, 0.7f); }
            const float excess = std::max(m.shelterSolid[c] - top, 0.5f * (m.shelterCrown[c] - top));
            if (excess > 0.3f && g > kUnknown * 0.5f) b = std::max(b, std::min(0.3f + excess / 8.0f, 1.0f));
            if (std::abs(x - ex) <= 1 && std::abs(z - ez) <= 1) r = gr = b = 1.0f;
            uint8_t* o = &row[size_t(px) * 3];
            o[0] = uint8_t(std::clamp(b, 0.0f, 1.0f) * 255.0f);
            o[1] = uint8_t(std::clamp(gr, 0.0f, 1.0f) * 255.0f);
            o[2] = uint8_t(std::clamp(r, 0.0f, 1.0f) * 255.0f);
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

}  // namespace rvk
