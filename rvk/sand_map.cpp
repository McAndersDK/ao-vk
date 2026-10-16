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
constexpr int kMapN = 256;                       // cells a side (128 units: the sand box reaches 48 ahead)
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

// The heights a triangle has over the cell [x0, x1] x [z0, z1] (its part over the cell: clipped against the cell's four
// sides) - false when none of it is over the cell. (A wall's triangle beside an arch's opening reaches down to the ground
// only where it stands, not over the opening.)
bool TriangleHeightsOverBox(const float a[3], const float b[3], const float c[3], float x0, float z0, float x1, float z1,
                            float* lo, float* hi)
{
    float poly[2][9][3];
    int count = 3, cur = 0;
    std::memcpy(poly[0][0], a, 12);
    std::memcpy(poly[0][1], b, 12);
    std::memcpy(poly[0][2], c, 12);
    for (int plane = 0; plane < 4 && count; ++plane) {
        const int axis = plane < 2 ? 0 : 2;
        const float bound = plane == 0 ? x0 : plane == 1 ? x1 : plane == 2 ? z0 : z1;
        const float sign = (plane & 1) ? -1.0f : 1.0f;           // (inside: sign * (v - bound) >= 0)
        int out = 0;
        for (int i = 0; i < count && out < 8; ++i) {
            const float* p = poly[cur][i];
            const float* q = poly[cur][(i + 1) % count];
            const float dp = sign * (p[axis] - bound), dq = sign * (q[axis] - bound);
            if (dp >= 0.0f) std::memcpy(poly[cur ^ 1][out++], p, 12);
            if ((dp >= 0.0f) != (dq >= 0.0f) && out < 8) {
                const float t = dp / (dp - dq);
                for (int k = 0; k < 3; ++k) poly[cur ^ 1][out][k] = p[k] + (q[k] - p[k]) * t;
                ++out;
            }
        }
        count = out;
        cur ^= 1;
    }
    if (!count) return false;
    *lo = 1e30f;
    *hi = -1e30f;
    for (int i = 0; i < count; ++i) {
        *lo = std::min(*lo, poly[cur][i][1]);
        *hi = std::max(*hi, poly[cur][i][1]);
    }
    return true;
}

// The shelter sweep over a grid (n x n, `cell` world units, the wind `w` towards x, z): out = max(top, out two cells
// upwind - the fall). Along the wind's larger axis, the cells two upwind are always done before (their bilinear corners
// lie a whole cell or more back along it). `edge`: the value upwind of the grid at a world x, z (or kUnknown).
template <typename Top, typename Edge>
void ShelterSweep(int n, float cell, const float w[2], float fallPerCell, Top top, Edge edge, std::vector<float>& out,
                  int rowFrom = 0, int rowTo = -1)
{
    if (rowFrom == 0) out.assign(size_t(n) * n, kUnknown);
    if (rowTo < 0) rowTo = n;
    const bool alongX = std::fabs(w[0]) >= std::fabs(w[1]);
    const int dirMajor = (alongX ? w[0] : w[1]) >= 0.0f ? 1 : -1;
    const float bx = -2.0f * w[0], bz = -2.0f * w[1];   // (two cells upwind)
    const float fall = 2.0f * fallPerCell;
    auto at = [&](int x, int z) { return out[size_t(z) * n + x]; };
    for (int i = rowFrom; i < rowTo; ++i) {
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
        const bool stale = m_frameClock - m_sandMap.built > 4.0 || m_sandMap.statelGen != m_statelGeneration ||
                           (m_sandMap.coverGen != m_coverMeshGen && m_frameClock - m_sandMap.built > 1.0);
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
            m.wallLo.assign(cells, 1e9f);
            m.floor.assign(cells, kUnknown);
            m.crownHi.assign(cells, kUnknown);
            m.cover.assign(cells, kUnknown);
            m.fine.assign(cells, 0);
            m.feed.assign(cells, 1.0f);
            m.statelGen = m_statelGeneration;
            const float x0 = float(ex) * kMapCell, z0 = float(ez) * kMapCell, size = kMapN * kMapCell;
            StatelItemsIn(x0, z0, x0 + size, z0 + size, job.items);
            m.coverGen = m_coverMeshGen;
            for (const auto& [key, c] : m_coverMeshes)
                if (c.boundsMax[0] >= x0 && c.boundsMin[0] <= x0 + size && c.boundsMax[2] >= z0 && c.boundsMin[2] <= z0 + size)
                    job.meshes.push_back(key);
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
                    m.fine[c] = 1;
                    const float r = float((colour >> 16) & 0xFF), g = float((colour >> 8) & 0xFF), b = float(colour & 0xFF);
                    if (g > r * 1.05f && g > b) m.feed[c] = 0.65f;   // (green: grass gives a little less sand)
                } else {
                    m.ground[c] = CoarseTerrainAt(wx, wz);
                }
            }
        }
        // The placed objects, an item at a time.
        while (job.row >= uint32_t(kMapN) && job.item < job.items.size() && NowMs() - start < kBudgetMs)
            RasterSandItem(m, job.items[job.item++]);
        // ... and what else the game draws standing still (remembered casters: a playfield's buildings, roofs).
        while (job.row >= uint32_t(kMapN) && job.item >= job.items.size() && job.mesh < job.meshes.size() &&
               NowMs() - start < kBudgetMs) {
            auto c = m_coverMeshes.find(job.meshes[job.mesh++]);
            if (c != m_coverMeshes.end())
                RasterSandTriangles(m, c->second.tri.data(), c->second.tri.size() / 9, c->second.seeThrough);
        }
        if (job.row >= uint32_t(kMapN) && job.item >= job.items.size() && job.mesh >= job.meshes.size() && !job.swept) {
            if (!job.nearest) {
                FinishSandCells(m);
                SandNearest(m);
                job.nearest = true;
                job.sweep = SandSweep{};
                job.sweep.active = true;
                std::memcpy(job.sweep.wind, m_windDir, sizeof(job.sweep.wind));
            }
            job.swept = StepSandSweep(job.sweep, m, start + kBudgetMs);
        }
        if (job.swept) {
            std::memcpy(m_sandShelterWind, job.sweep.wind, sizeof(m_sandShelterWind));
            ++m_sandShelterGen;
            m_sandSweep.active = false;          // (one for the old map: dropped)
            m.valid = true;
            m.built = m_frameClock;
            std::swap(m_sandMap, m_sandMapNext);
            job.active = false;
            static uint32_t logged = 0;
            if (logged < 8 && ++logged) {
                size_t solid = 0, crown = 0, ground = 0;
                for (size_t c = 0; c < m_sandMap.ground.size(); ++c) {
                    ground += m_sandMap.ground[c] > kUnknown * 0.5f;
                    solid += m_sandMap.solid[c] > m_sandMap.ground[c] + 0.3f;
                    crown += m_sandMap.crownHi[c] > kUnknown * 0.5f;
                }
                Log("sand map: %d x %d cells of %.1f round (%.0f %.0f): ground known %.0f%%, solid %.1f%%, see-through "
                    "%.1f%%; %zu placed pieces, %zu other meshes (%zu triangles kept)", kMapN, kMapN, double(kMapCell), double(m_frameEye[0]),
                    double(m_frameEye[2]), 100.0 * double(ground) / double(m_sandMap.ground.size()),
                    100.0 * double(solid) / double(m_sandMap.ground.size()),
                    100.0 * double(crown) / double(m_sandMap.ground.size()), job.items.size(), job.meshes.size(),
                    m_coverTriangles);
            }
        }
    }
    const float* w = m_windDir;
    auto turned = [&](const float a[2]) { return a[0] * w[0] + a[1] * w[1] < 0.99939f; };   // (two degrees)
    // The coarse terrain's shelter (the hills upwind of the map): again when the wind has turned five degrees or the
    // grid moved or filled (at most every three seconds), 32 rows a frame into a pending grid, swapped in when done.
    auto turnedBy = [&](const float a[2], float cosine) { return a[0] * w[0] + a[1] * w[1] < cosine; };
    if (!m_tsHeights.empty() && !m_sandCoarseJob &&
        (turnedBy(m_sandCoarseWind, 0.9962f) || m_sandCoarseOrigin[0] != m_tsOrigin[0] ||
         m_sandCoarseOrigin[1] != m_tsOrigin[1] || m_tsDirty) &&
        m_frameClock - m_sandCoarseMade > 3.0) {
        m_sandCoarseJob = true;
        m_sandCoarseRow = 0;
        std::memcpy(m_sandCoarseJobWind, w, sizeof(m_sandCoarseJobWind));
        m_sandCoarseJobOrigin[0] = m_tsOrigin[0];
        m_sandCoarseJobOrigin[1] = m_tsOrigin[1];
        m_sandCoarseMade = m_frameClock;
    }
    if (m_sandCoarseJob) {
        if (m_sandCoarseJobOrigin[0] != m_tsOrigin[0] || m_sandCoarseJobOrigin[1] != m_tsOrigin[1]) {
            m_sandCoarseJob = false;             // (the grid moved meanwhile: started again later)
        } else {
            const int to = std::min(m_sandCoarseRow + 32, kCoarseN);
            ShelterSweep(kCoarseN, kCoarseCell, m_sandCoarseJobWind, kCoarseCell / kSolidLee,
                         [&](int x, int z) { return m_tsHeights[size_t(z) * kCoarseN + x]; },
                         [](float, float) { return kUnknown; }, m_sandCoarsePending, m_sandCoarseRow, to);
            m_sandCoarseRow = to;
            if (to >= kCoarseN) {
                m_sandCoarseShelter.swap(m_sandCoarsePending);
                m_sandCoarseOrigin[0] = m_sandCoarseJobOrigin[0];
                m_sandCoarseOrigin[1] = m_sandCoarseJobOrigin[1];
                std::memcpy(m_sandCoarseWind, m_sandCoarseJobWind, sizeof(m_sandCoarseWind));
                m_sandCoarseJob = false;
                m_sandShelterWind[0] = m_sandShelterWind[1] = 0.0f;   // (the map's swept again with it)
            }
        }
    }
    // The map's shelter and flow, again when the wind has turned two degrees (at most twice a second), in slices.
    if (m_sandMap.valid && !m_sandSweep.active && turned(m_sandShelterWind) && m_frameClock - m_sandShelterMade > 0.5) {
        m_sandSweep = SandSweep{};
        m_sandSweep.active = true;
        std::memcpy(m_sandSweep.wind, w, sizeof(m_sandSweep.wind));
        m_sandShelterMade = m_frameClock;
    }
    if (m_sandSweep.active && StepSandSweep(m_sandSweep, m_sandMap, start + kBudgetMs)) {   // (the one budget)
        std::memcpy(m_sandShelterWind, m_sandSweep.wind, sizeof(m_sandShelterWind));
        ++m_sandShelterGen;
    }
    const double took = NowMs() - start;
    m_sandMapMs += took;
    m_sandMapMaxMs = std::max(m_sandMapMaxMs, took);
    if (m_frameNumber % 600 == 300) {
        Log("sand map: %.3f ms a frame, at most %.2f (build, shelter, flow: last 600 frames); eye's shelter %.2f",
            m_sandMapMs / 600.0, m_sandMapMaxMs, double(m_sandEyeShelter));
        m_sandMapMs = m_sandMapMaxMs = 0.0;
    }
}

// Once a map's pieces are in: the floors raise the ground; a cell is solid only where walls reach down to its foot
// (within 1.5 units of its ground) - under an arch, a bridge, a roof the air and the sand go through (the user: the
// storm stopped at an arch); a room shut in by walls is sheltered by them instead.
void Device::FinishSandCells(SandMap& m)
{
    for (size_t c = 0; c < m.ground.size(); ++c) {
        if (m.floor[c] > m.ground[c]) m.ground[c] = m.floor[c];
        const bool blocks = m.ground[c] > kUnknown * 0.5f && m.wallLo[c] <= m.ground[c] + 1.5f;
        if (!blocks) m.solid[c] = kUnknown;
    }
    // The ground's hollows (puddles): how far each cell lies below the mean of the ground ~4 units round it (two
    // running sums, along x then z; unknown ground counts as none).
    const int r = 8;
    std::vector<float> sum(m.ground.size(), 0.0f), count(m.ground.size(), 0.0f), sum2(m.ground.size()), count2(m.ground.size());
    for (int z = 0; z < kMapN; ++z) {
        float s = 0.0f, n = 0.0f;
        // (Only the ground captured close up: mixed with the coarse grid's, the seam between them - a ring round the
        // eye - read as hollows, and its puddles travelled with a running character, the user.)
        auto at = [&](int x, float& v) {
            const size_t i = size_t(z) * kMapN + x;
            const float g = m.ground[i];
            if (g > kUnknown * 0.5f && i < m.fine.size() && m.fine[i]) { v = g; return true; }
            return false;
        };
        for (int x = -r; x < kMapN; ++x) {
            float v;
            if (x + r < kMapN && at(x + r, v)) { s += v; n += 1.0f; }
            if (x - r - 1 >= 0 && at(x - r - 1, v)) { s -= v; n -= 1.0f; }
            if (x >= 0) { sum[size_t(z) * kMapN + x] = s; count[size_t(z) * kMapN + x] = n; }
        }
    }
    for (int x = 0; x < kMapN; ++x) {
        float s = 0.0f, n = 0.0f;
        for (int z = -r; z < kMapN; ++z) {
            if (z + r < kMapN) { s += sum[size_t(z + r) * kMapN + x]; n += count[size_t(z + r) * kMapN + x]; }
            if (z - r - 1 >= 0) { s -= sum[size_t(z - r - 1) * kMapN + x]; n -= count[size_t(z - r - 1) * kMapN + x]; }
            if (z >= 0) { sum2[size_t(z) * kMapN + x] = s; count2[size_t(z) * kMapN + x] = n; }
        }
    }
    m.hollow.assign(m.ground.size(), 0.0f);
    const float full = float((2 * r + 1) * (2 * r + 1));
    for (size_t c = 0; c < m.ground.size(); ++c)
        if (m.fine[c] && count2[c] > 0.6f * full) {   // (most of the cells round it close up too - fading in: no edge)
            const float k = std::clamp((count2[c] / full - 0.6f) / 0.35f, 0.0f, 1.0f);
            m.hollow[c] = std::max(sum2[c] / count2[c] - m.ground[c], 0.0f) * k * k * (3.0f - 2.0f * k);
        }
}

// The flow round solid obstacles: each cell's nearest one (a distance transform: the nearest solid cell carried along
// in two passes), and near it (within kFlowReach) the wind's part heading into it taken away and as much added along
// its side - the air going round a trunk, a wall's end, a boulder; then one sweep along the wind adds up how far the
// air arriving at each cell was pushed aside on its way (relaxing over kFlowRelax), so the sand's streaks part round an
// obstacle and close behind it. All up to the obstacle's top (the shader: below nearTop).
void Device::SandNearest(SandMap& m)
{
    const size_t cells = size_t(kMapN) * kMapN;
    std::vector<int16_t> sx(cells, -1), sz(cells, -1);
    for (int z = 0; z < kMapN; ++z)
        for (int x = 0; x < kMapN; ++x) {
            const size_t c = size_t(z) * kMapN + x;
            if (m.ground[c] > kUnknown * 0.5f && m.solid[c] > m.ground[c] + 0.3f) { sx[c] = int16_t(x); sz[c] = int16_t(z); }
        }
    auto relax = [&](int x, int z, int nx, int nz) {
        if (nx < 0 || nz < 0 || nx >= kMapN || nz >= kMapN) return;
        const size_t c = size_t(z) * kMapN + x, n = size_t(nz) * kMapN + nx;
        if (sx[n] < 0) return;
        const int dx = x - sx[n], dz = z - sz[n];
        if (sx[c] >= 0) {
            const int ox = x - sx[c], oz = z - sz[c];
            if (ox * ox + oz * oz <= dx * dx + dz * dz) return;
        }
        sx[c] = sx[n];
        sz[c] = sz[n];
    };
    for (int z = 0; z < kMapN; ++z)
        for (int x = 0; x < kMapN; ++x) {
            relax(x, z, x - 1, z); relax(x, z, x, z - 1); relax(x, z, x - 1, z - 1); relax(x, z, x + 1, z - 1);
        }
    for (int z = kMapN - 1; z >= 0; --z)
        for (int x = kMapN - 1; x >= 0; --x) {
            relax(x, z, x + 1, z); relax(x, z, x, z + 1); relax(x, z, x + 1, z + 1); relax(x, z, x - 1, z + 1);
        }
    m.nearTop.assign(cells, kUnknown);
    m.nearDX.assign(cells, 1e9f);
    m.nearDZ.assign(cells, 1e9f);
    m.anySolid = false;
    for (size_t c = 0; c < cells; ++c) {
        if (sx[c] < 0) continue;
        const size_t s = size_t(sz[c]) * kMapN + sx[c];
        m.nearTop[c] = m.solid[s];
        m.nearDX[c] = float(int(c % kMapN) - sx[c]) * kMapCell;   // (from it to here, world)
        m.nearDZ[c] = float(int(c / kMapN) - sz[c]) * kMapCell;
        m.anySolid = true;
    }
}

namespace {
constexpr float kFlowReach = 3.0f, kFlowRelax = 12.0f;
}

// The wind's direction round solid obstacles, every cell (into the sweep's own vectors).
void Device::SandFlowCells(const SandMap& m, SandSweep& j)
{
    const float* w = j.wind;
    const size_t cells = size_t(kMapN) * kMapN;
    j.flowX.assign(cells, w[0]);
    j.flowZ.assign(cells, w[1]);
    if (!m.anySolid || m.nearDX.size() != cells)
        return;
    for (size_t c = 0; c < cells; ++c) {
        const float d = std::hypot(m.nearDX[c], m.nearDZ[c]);
        if (d >= kFlowReach) continue;
        if (d < 1e-3f) { j.flowX[c] = j.flowZ[c] = 0.0f; continue; }   // (in it)
        const float nx = m.nearDX[c] / d, nz = m.nearDZ[c] / d;         // (away from it)
        float k = 1.0f - d / kFlowReach;
        k = k * k * (3.0f - 2.0f * k);
        float vx = w[0], vz = w[1];
        const float vn = vx * nx + vz * nz;
        if (vn < 0.0f) {                         // (heading into it: that part taken away, as much added sideways)
            vx -= nx * vn * k;
            vz -= nz * vn * k;
            float tx = vx, tz = vz;
            const float tl = std::hypot(tx, tz);
            if (tl > 1e-4f) { tx /= tl; tz /= tl; }
            vx += tx * (-vn) * k * 0.6f;
            vz += tz * (-vn) * k * 0.6f;
        }
        j.flowX[c] = vx;
        j.flowZ[c] = vz;
    }
}

// How far the air was pushed aside, rows [from, to) of the sweep along the wind (two cells a step, as the shelter's).
void Device::SandOffsetRows(SandSweep& j, int from, int to)
{
    const float* w = j.wind;
    const size_t cells = size_t(kMapN) * kMapN;
    if (from == 0) {
        j.offX.assign(cells, 0.0f);
        j.offZ.assign(cells, 0.0f);
    }
    const float decay = std::exp(-2.0f * kMapCell / kFlowRelax), step = 2.0f * kMapCell;
    const bool alongX = std::fabs(w[0]) >= std::fabs(w[1]);
    const int dirMajor = (alongX ? w[0] : w[1]) >= 0.0f ? 1 : -1;
    for (int i = from; i < to; ++i) {
        const int major = dirMajor > 0 ? i : kMapN - 1 - i;
        for (int minor = 0; minor < kMapN; ++minor) {
            const int x = alongX ? major : minor, z = alongX ? minor : major;
            const size_t c = size_t(z) * kMapN + x;
            const float px = float(x) - 2.0f * w[0], pz = float(z) - 2.0f * w[1];
            float ox = 0.0f, oz = 0.0f;
            if (px >= 0.0f && pz >= 0.0f && px <= float(kMapN - 1) && pz <= float(kMapN - 1)) {
                const int x0 = int(px), z0 = int(pz), x1 = std::min(x0 + 1, kMapN - 1), z1 = std::min(z0 + 1, kMapN - 1);
                const float fx = px - float(x0), fz = pz - float(z0);
                auto lerp = [&](const std::vector<float>& a) {
                    return (a[size_t(z0) * kMapN + x0] * (1 - fx) + a[size_t(z0) * kMapN + x1] * fx) * (1 - fz) +
                           (a[size_t(z1) * kMapN + x0] * (1 - fx) + a[size_t(z1) * kMapN + x1] * fx) * fz;
                };
                ox = lerp(j.offX) * decay;
                oz = lerp(j.offZ) * decay;
            }
            j.offX[c] = ox + (j.flowX[c] - w[0]) * step;
            j.offZ[c] = oz + (j.flowZ[c] - w[1]) * step;
        }
    }
}

// A map's shelter and flow for a wind, a slice at a time (rows of kMapN; until `deadline`): the solid shelter, the
// see-through one, the flow, the push aside. True when done - then they are the map's (the old kept till then: no
// flicker while it is made).
bool Device::StepSandSweep(SandSweep& j, SandMap& m, double deadline)
{
    constexpr int kRows = 16;
    const float ox = float(m.origin[0]) * kMapCell, oz = float(m.origin[1]) * kMapCell;
    auto top = [&](int x, int z) {
        const size_t c = size_t(z) * kMapN + x;
        return std::max(m.ground[c], m.solid[c]);
    };
    auto edge = [&](float lx, float lz) {         // (upwind of the map: the coarse terrain's, a unit lower - its cells
        return CoarseShelterAt(ox + lx, oz + lz) - 1.0f;   // hold their highest point)
    };
    auto crown = [&](int x, int z) { return m.crownHi[size_t(z) * kMapN + x]; };
    auto none = [](float, float) { return kUnknown; };
    while (NowMs() < deadline) {
        const int to = std::min(j.row + kRows, kMapN);
        if (j.stage == 0) ShelterSweep(kMapN, kMapCell, j.wind, kMapCell / kSolidLee, top, edge, j.solid, j.row, to);
        else if (j.stage == 1) ShelterSweep(kMapN, kMapCell, j.wind, kMapCell / kCrownLee, crown, none, j.crown, j.row, to);
        else {
            if (j.row == 0) SandFlowCells(m, j);
            SandOffsetRows(j, j.row, to);
        }
        j.row = to;
        if (j.row < kMapN) continue;
        j.row = 0;
        if (++j.stage < 3) continue;
        m.shelterSolid.swap(j.solid);
        m.shelterCrown.swap(j.crown);
        m.flowX.swap(j.flowX);
        m.flowZ.swap(j.flowZ);
        m.offsetX.swap(j.offX);
        m.offsetZ.swap(j.offZ);
        j.active = false;
        return true;
    }
    return false;
}

constexpr float kRoofOpaque = 0.8f;

// A placed piece rasterised into the map from above.
void Device::RasterSandItem(SandMap& m, uint32_t index)
{
    const StatelItem& it = m_statelItems[index];
    const StatelPiece& p = m_statelPieces[it.piece];
    // (A cut-out mostly whole - a roof's, a grating's - keeps the rain off as a solid one; a canopy's leaves are seen
    // through, the wind and the rain through them slowed.)
    const bool seeThrough = p.alphaRef >= 0.0f && p.opaque < kRoofOpaque;
    if (uint64_t(p.firstVertex) + p.vertexCount > m_statelCpuPositions.size() / 3 ||
        uint64_t(p.firstIndex) + p.indexCount > m_statelCpuIndices.size())
        return;
    const auto& w = it.world.m;
    std::vector<float> tri;
    tri.reserve(size_t(p.indexCount) * 3);
    for (uint32_t t = 0; t + 2 < p.indexCount; t += 3) {
        const uint16_t* idx = &m_statelCpuIndices[p.firstIndex + t];
        if (idx[0] >= p.vertexCount || idx[1] >= p.vertexCount || idx[2] >= p.vertexCount)
            continue;
        for (int k = 0; k < 3; ++k) {
            const float* v = &m_statelCpuPositions[(size_t(p.firstVertex) + idx[k]) * 3];
            for (int j = 0; j < 3; ++j) tri.push_back(v[0] * w[0][j] + v[1] * w[1][j] + v[2] * w[2][j] + w[3][j]);
        }
    }
    RasterSandTriangles(m, tri.data(), tri.size() / 9, seeThrough);
}

void Device::RasterSandTriangles(SandMap& m, const float* tri, size_t triangles, bool seeThrough)
{
    const float ox = float(m.origin[0]) * kMapCell, oz = float(m.origin[1]) * kMapCell;
    for (size_t t = 0; t < triangles; ++t) {
        const float* a = tri + t * 9;
        const float* b = a + 3;
        const float* c = a + 6;
        const float a2[2] = {a[0] - ox, a[2] - oz}, b2[2] = {b[0] - ox, b[2] - oz}, c2[2] = {c[0] - ox, c[2] - oz};
        const int x0 = std::max(int(std::floor(std::min({a2[0], b2[0], c2[0]}) / kMapCell)), 0);
        const int x1 = std::min(int(std::floor(std::max({a2[0], b2[0], c2[0]}) / kMapCell)), kMapN - 1);
        const int z0 = std::max(int(std::floor(std::min({a2[1], b2[1], c2[1]}) / kMapCell)), 0);
        const int z1 = std::min(int(std::floor(std::max({a2[1], b2[1], c2[1]}) / kMapCell)), kMapN - 1);
        if (x0 > x1 || z0 > z1)
            continue;
        // Flat (a floor, a roof) or not (a wall, a slope, an arch's curve).
        const float e1[3] = {b[0] - a[0], b[1] - a[1], b[2] - a[2]}, e2[3] = {c[0] - a[0], c[1] - a[1], c[2] - a[2]};
        const float nx = e1[1] * e2[2] - e1[2] * e2[1], ny = e1[2] * e2[0] - e1[0] * e2[2], nz = e1[0] * e2[1] - e1[1] * e2[0];
        const float nl = std::sqrt(nx * nx + ny * ny + nz * nz);
        const bool flat = nl > 1e-9f && std::fabs(ny) > 0.8f * nl;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                if (!TriangleTouchesBox(a2, b2, c2, float(x) * kMapCell, float(z) * kMapCell, float(x + 1) * kMapCell,
                                        float(z + 1) * kMapCell))
                    continue;
                float lo, hi;
                if (!TriangleHeightsOverBox(a, b, c, ox + float(x) * kMapCell, oz + float(z) * kMapCell,
                                            ox + float(x + 1) * kMapCell, oz + float(z + 1) * kMapCell, &lo, &hi)) {
                    // (A wall exactly along the cell's side: clipped to a line - its own heights.)
                    lo = std::min({a[1], b[1], c[1]});
                    hi = std::max({a[1], b[1], c[1]});
                }
                const size_t cell = size_t(z) * kMapN + x;
                if (seeThrough) {
                    m.crownHi[cell] = std::max(m.crownHi[cell], hi);
                    m.crownLo[cell] = std::min(m.crownLo[cell], lo);
                } else if (flat && m.ground[cell] > kUnknown * 0.5f && hi <= m.ground[cell] + 2.0f) {
                    m.floor[cell] = std::max(m.floor[cell], hi);   // (a floor: plating, a platform, steps)
                } else {
                    m.cover[cell] = std::max(m.cover[cell], hi);   // (the rain's: kept whether it reaches down or not)
                    m.solid[cell] = std::max(m.solid[cell], hi);
                    if (!flat) m.wallLo[cell] = std::min(m.wallLo[cell], lo);
                }
            }
    }
}

// A remembered static caster's triangles kept for the map (world space; triangle lists and strips). Not the ground,
// characters, plants with leaves; a cut-out one (its texture with holes) as see-through. Bounded in all.
void Device::AddCoverMesh(uint64_t key, const CachedCaster& e, const void* vertices, const uint16_t* indices)
{
    constexpr size_t kMostTriangles = 400000, kMostOne = 20000;
    const VisualKind kind = VisualKind(e.kind);
    if ((kind != VisualKind::Static && kind != VisualKind::Unknown) || e.leafKey || (e.primitive != 4 && e.primitive != 5))
        return;
    const size_t n = e.indexCount ? e.indexCount : e.vertexCount;
    const size_t triangles = e.primitive == 4 ? n / 3 : n >= 3 ? n - 2 : 0;
    if (!triangles || triangles > kMostOne || m_coverTriangles + triangles > kMostTriangles)
        return;
    CoverMesh c;
    std::memcpy(c.boundsMin, e.boundsMin, sizeof(c.boundsMin));
    std::memcpy(c.boundsMax, e.boundsMax, sizeof(c.boundsMax));
    c.seeThrough = e.texture && !e.texture->m_opaque && e.alphaRef >= 0.0f;
    c.tri.reserve(triangles * 9);
    const uint8_t* v = static_cast<const uint8_t*>(vertices);
    const auto& w = e.world.m;
    auto put = [&](size_t i) {
        const uint32_t k = e.indexCount ? indices[i] : uint32_t(i);
        if (k >= e.vertexCount) { for (int j = 0; j < 3; ++j) c.tri.push_back(0.0f); return false; }
        float p[3];
        std::memcpy(p, v + size_t(k) * e.stride, sizeof(p));
        for (int j = 0; j < 3; ++j) c.tri.push_back(p[0] * w[0][j] + p[1] * w[1][j] + p[2] * w[2][j] + w[3][j]);
        return true;
    };
    for (size_t t = 0; t < triangles; ++t) {
        const size_t base = e.primitive == 4 ? t * 3 : t;
        const size_t before = c.tri.size();
        if (!(put(base) & put(base + 1) & put(base + 2)))
            c.tri.resize(before);                // (an index out of range: left out)
    }
    if (c.tri.empty())
        return;
    m_coverTriangles += c.tri.size() / 9;
    auto old = m_coverMeshes.find(key);
    if (old != m_coverMeshes.end()) m_coverTriangles -= old->second.tri.size() / 9;
    m_coverMeshes[key] = std::move(c);
    ++m_coverMeshGen;
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
    const bool flow = m.flowX.size() == m.ground.size();
    out->flow[0] = flow ? m.flowX[c] : 0.0f;
    out->flow[1] = flow ? m.flowZ[c] : 0.0f;
    out->offset[0] = flow ? m.offsetX[c] : 0.0f;
    out->offset[1] = flow ? m.offsetZ[c] : 0.0f;
    out->nearTop = flow ? m.nearTop[c] : kUnknown;
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

// The map's solid shelter for the sand fog (sand_fog.frag: the dust held off in the lee of walls and hills near the
// eye), made again when the shelter is: per cell R the solid shelter's height over the cell's top (0: none), G the top
// over m_sandFogShelterRef (half floats). (See-through obstacles left out: a canopy holds off the blown sand, not the
// dust in the air.)
bool Device::SandFogShelter(float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const SandMap& m = m_sandMap;
    if (!m.valid || m.shelterSolid.size() != size_t(kMapN) * kMapN)
        return false;
    if (!m_sandFogShelter) {
        m_sandFogShelter = CreateTexture(kMapN, kMapN, Format::RG16F, 1);
        if (!m_sandFogShelter)
            return false;
        m_sandFogShelterGen = ~0u;
    }
    if (m_sandFogShelterGen != m_sandShelterGen) {
        auto half = [](float f) {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
            const uint32_t mant = x & 0x7FFFFFu;
            if (e <= 0) return uint16_t(sign);
            if (e >= 31) return uint16_t(sign | 0x7BFFu);
            return uint16_t(sign | (uint32_t(e) << 10) | (mant >> 13));
        };
        m_sandFogShelterRef = m_frameEye[1];
        std::vector<uint16_t> texels(size_t(kMapN) * kMapN * 2);
        for (size_t c = 0; c < size_t(kMapN) * kMapN; ++c) {
            const bool known = m.ground[c] > kUnknown * 0.5f;
            const float top = known ? std::max(m.ground[c], m.solid[c]) : m_sandFogShelterRef;
            const float excess = known && m.shelterSolid[c] > kUnknown * 0.5f ? std::max(m.shelterSolid[c] - top, 0.0f) : 0.0f;
            texels[c * 2] = half(excess);
            texels[c * 2 + 1] = half(top - m_sandFogShelterRef);
        }
        UpdateTexture(m_sandFogShelter, 0, 0, 0, kMapN, kMapN, texels.data(), kMapN * 4);
        m_sandFogShelterGen = m_sandShelterGen;
    }
    out[0] = float(m.origin[0]) * kMapCell;
    out[1] = float(m.origin[1]) * kMapCell;
    out[2] = 1.0f / (float(kMapN) * kMapCell);
    out[3] = m_sandFogShelterRef;
    return true;
}

// The rain's cover map (rain.vert, splash.vert, ffp_main's wet ground), made again when the map is: per cell R the
// highest solid thing over it, G the see-through cover's top, B the ground (over m_rainCoverRef; -9000 none), A how
// far its ground lies in a hollow. Built whenever it rains or the ground is wet (rain.cpp), not only in a sandstorm.
bool Device::RainCoverMap(float out[4])
{
    out[0] = out[1] = out[2] = out[3] = 0.0f;
    const SandMap& m = m_sandMap;
    if (!m.valid || m.cover.size() != size_t(kMapN) * kMapN || m.hollow.size() != m.cover.size())
        return false;
    if (!m_rainCover) {
        m_rainCover = CreateTexture(kMapN, kMapN, Format::RGBA16F, 1);
        if (!m_rainCover)
            return false;
        m_rainCoverGen = ~0u;
    }
    if (m_rainCoverGen != m_sandShelterGen) {
        auto half = [](float f) {
            uint32_t x;
            std::memcpy(&x, &f, 4);
            const uint32_t sign = (x >> 16) & 0x8000u;
            const int32_t e = int32_t((x >> 23) & 0xFF) - 127 + 15;
            const uint32_t mant = x & 0x7FFFFFu;
            if (e <= 0) return uint16_t(sign);
            if (e >= 31) return uint16_t(sign | 0x7BFFu);
            return uint16_t(sign | (uint32_t(e) << 10) | (mant >> 13));
        };
        m_rainCoverRef = m_frameEye[1];
        auto rel = [&](float y) { return y > kUnknown * 0.5f && y < 1e8f ? y - m_rainCoverRef : -9000.0f; };
        std::vector<uint16_t> texels(size_t(kMapN) * kMapN * 4);
        for (size_t c = 0; c < size_t(kMapN) * kMapN; ++c) {
            texels[c * 4 + 0] = half(rel(m.cover[c]));
            texels[c * 4 + 1] = half(rel(m.crownHi[c]));
            texels[c * 4 + 2] = half(rel(m.ground[c]));   // (the ground: a storm's debris skims it)
            texels[c * 4 + 3] = half(std::min(m.hollow[c], 8.0f));
        }
        UpdateTexture(m_rainCover, 0, 0, 0, kMapN, kMapN, texels.data(), kMapN * 8);
        m_rainCoverGen = m_sandShelterGen;
    }
    out[0] = float(m.origin[0]) * kMapCell;
    out[1] = float(m.origin[1]) * kMapCell;
    out[2] = 1.0f / (float(kMapN) * kMapCell);
    out[3] = m_rainCoverRef;
    return true;
}

// The frame dump: what keeps the rain off at a world x, z - the map's cell (solid cover, canopy, ground) and every
// placed piece whose box stands over it (cut-out or not, how much of its mask is whole, how it counts).
void Device::DumpRainCover(std::FILE* f, float x, float z)
{
    const SandMap& m = m_sandMap;
    const int cx = int(std::floor(x / kMapCell)) - m.origin[0], cz = int(std::floor(z / kMapCell)) - m.origin[1];
    if (!m.valid || cx < 0 || cz < 0 || cx >= kMapN || cz >= kMapN || m.cover.size() != size_t(kMapN) * kMapN) {
        std::fprintf(f, "# rain cover at %.1f %.1f: no map (valid %d)\n", double(x), double(z), int(m.valid));
        return;
    }
    const size_t c = size_t(cz) * kMapN + cx;
    auto v = [](float y) { return y > kUnknown * 0.5f && y < 1e8f ? double(y) : -1.0; };
    std::fprintf(f, "# rain cover at %.1f %.1f (cell %d %d): cover %.2f, canopy %.2f, ground %.2f, solid %.2f, floor %.2f, "
                    "hollow %.2f; wetness %.2f puddles %.2f rain %.2f (-1: none)\n",
                 double(x), double(z), cx, cz, v(m.cover[c]), v(m.crownHi[c]), v(m.ground[c]), v(m.solid[c]),
                 v(m.floor[c]), double(m.hollow.size() > c ? m.hollow[c] : 0.0f), double(m_wetness), double(m_puddles),
                 double(m_rain));
    std::vector<uint32_t> items;
    StatelItemsIn(x - 0.25f, z - 0.25f, x + 0.25f, z + 0.25f, items);
    for (uint32_t i : items) {
        const StatelItem& it = m_statelItems[i];
        const StatelPiece& p = m_statelPieces[it.piece];
        if (x < it.boundsMin[0] || x > it.boundsMax[0] || z < it.boundsMin[2] || z > it.boundsMax[2])
            continue;
        std::fprintf(f, "#   piece %u (statel %u, kind %u): y %.2f..%.2f, %s alpha ref %.2f, %.0f%% of its mask whole -> %s\n",
                     it.piece, it.statel, unsigned(it.kind), double(it.boundsMin[1]), double(it.boundsMax[1]),
                     p.alphaRef >= 0.0f ? "cut-out" : "solid", double(p.alphaRef), double(p.opaque) * 100.0,
                     p.alphaRef >= 0.0f && p.opaque < kRoofOpaque ? "see-through (canopy)" : "solid");
    }
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
    // ... and the flow round obstacles (<dump>-flow.bmp): how far the air was pushed aside, x red, z green (grey: none),
    // the obstacles blue.
    if (m.offsetX.size() != m.ground.size())
        return;
    std::string flowPath = path;
    const size_t at = flowPath.rfind("-sand.bmp");
    if (at == std::string::npos)
        return;
    flowPath.replace(at, 9, "-flow.bmp");
    file = std::fopen(flowPath.c_str(), "wb");
    if (!file)
        return;
    std::fwrite(header, 1, 54, file);
    for (int py = 0; py < wpx; ++py) {
        const int z = py / s;
        for (int px = 0; px < wpx; ++px) {
            const size_t c = size_t(z) * kMapN + px / s;
            uint8_t* o = &row[size_t(px) * 3];
            const bool solid = m.solid[c] > m.ground[c] + 0.3f;
            o[0] = solid ? 255 : 128;
            o[1] = uint8_t(std::clamp(0.5f + m.offsetZ[c] / 3.0f, 0.0f, 1.0f) * 255.0f);
            o[2] = uint8_t(std::clamp(0.5f + m.offsetX[c] / 3.0f, 0.0f, 1.0f) * 255.0f);
            if (solid) o[1] = o[2] = 40;
        }
        std::fwrite(row.data(), 1, row.size(), file);
    }
    std::fclose(file);
}

}  // namespace rvk
