// The suns (RVK_Sun; sun.frag). Rubi-Ka has two suns, and the game shows each only as additive ray sprites
// (VisualSunRays_t: four a frame, two a sun - a small bright one, a big dim one; tinted - a white sun, a bluish purple
// one) - no disc; our cloud sheets and layer, drawn after them, left little of them. Each frame the sprites are noted
// (where - their fan's hub -, their outline - the reach of their spikes at 32 angles round it -, their colour) and
// grouped into suns; at the first sprite of the next frame each sun is drawn into the sky, before the game's sheets
// and the cloud layer, which cover it as thick as they are; at the sky's end a share of its glow again, over the
// clouds (RVK_SunBleed). The look rebuilt as AO's, not a real sun's (user, 2026-10-13): a big spiky star of the
// sprites' own outline, a white-hot core of the small sprite's, a few broad beams; the spikes swelling, the beams
// turning slowly; a bit tamer than the game's (RVK_SunSize). The game's own sprites off by default (RVK_GameSun).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

// A sun sprite: a fan of triangles round its sun (the hub: the vertex most triangles share - the middle of the
// rays; their bounding box or mean is not, the rays being of many lengths). Where (the hub through its matrix, from
// the eye), how big (its farthest vertex from the hub, over the distance), its colour (the vertices' mean).
void Device::NoteSunSprite(uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                           uint32_t indexCount)
{
    if (m_sunSpritesFrame != m_frameNumber) {    // (a new frame: the last one's kept until now - DrawSun read them)
        m_sunSprites.clear();
        m_sunSpritesFrame = m_frameNumber;
    }
    if (!vertices || vertexCount == 0 || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || m_sunSprites.size() >= 8)
        return;
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    uint32_t hub = 0;
    if (indices && indexCount > 0 && vertexCount <= 256) {
        uint16_t uses[256] = {};
        for (uint32_t i = 0; i < indexCount; ++i)
            if (indices[i] < vertexCount && ++uses[indices[i]] > uses[hub]) hub = indices[i];
    }
    float c[3], col[3] = {};
    std::memcpy(c, base + size_t(hub) * layout.stride + layout.offset[0], sizeof(c));
    float reach = 0.0f;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint8_t* v = base + size_t(i) * layout.stride;
        float p[3];
        std::memcpy(p, v + layout.offset[0], sizeof(p));
        reach = std::max(reach, (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]));
        if (layout.offset[2] >= 0) {
            uint32_t argb;
            std::memcpy(&argb, v + layout.offset[2], 4);
            col[0] += float((argb >> 16) & 0xFF) / 255.0f / float(vertexCount);
            col[1] += float((argb >> 8) & 0xFF) / 255.0f / float(vertexCount);
            col[2] += float(argb & 0xFF) / 255.0f / float(vertexCount);
        }
    }
    const auto& w = m_world.m;
    const auto& v = m_view.m;
    float d[3];
    for (int j = 0; j < 3; ++j) {
        const float wc = c[0] * w[0][j] + c[1] * w[1][j] + c[2] * w[2][j] + w[3][j];
        const float eye = -(v[3][0] * v[j][0] + v[3][1] * v[j][1] + v[3][2] * v[j][2]);
        d[j] = wc - eye;
    }
    const float l = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    if (l < 1e-3f)
        return;
    const float scale = std::sqrt(w[0][0] * w[0][0] + w[0][1] * w[0][1] + w[0][2] * w[0][2]);
    SunSprite s{};
    for (int j = 0; j < 3; ++j) s.dir[j] = d[j] / l;
    s.size = std::sqrt(reach) * scale / l;
    if (layout.offset[2] < 0) col[0] = col[1] = col[2] = 1.0f;
    std::memcpy(s.colour, col, sizeof(col));
    s.hub = hub;
    // Its outline: each rim vertex's angle round the hub (in the sun's own frame: as sun.frag's phi) and its reach
    // (radians), resampled to kSunBins even angles (between two vertices: a straight edge's reach, near enough linear).
    float up[3] = {0.0f, 1.0f, 0.0f};
    if (std::fabs(s.dir[1]) >= 0.99f) up[0] = 1.0f, up[1] = 0.0f;
    float u[3] = {up[1] * s.dir[2] - up[2] * s.dir[1], up[2] * s.dir[0] - up[0] * s.dir[2], up[0] * s.dir[1] - up[1] * s.dir[0]};
    const float ul = std::sqrt(u[0] * u[0] + u[1] * u[1] + u[2] * u[2]);
    for (float& x : u) x /= ul;
    const float vv[3] = {s.dir[1] * u[2] - s.dir[2] * u[1], s.dir[2] * u[0] - s.dir[0] * u[2], s.dir[0] * u[1] - s.dir[1] * u[0]};
    std::vector<std::pair<float, float>> rim;
    rim.reserve(vertexCount);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        if (i == hub)
            continue;
        float p[3], o[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        for (int j = 0; j < 3; ++j)
            o[j] = (p[0] - c[0]) * w[0][j] + (p[1] - c[1]) * w[1][j] + (p[2] - c[2]) * w[2][j];
        const float x = o[0] * u[0] + o[1] * u[1] + o[2] * u[2], y = o[0] * vv[0] + o[1] * vv[1] + o[2] * vv[2];
        rim.emplace_back(std::atan2(y, x), std::sqrt(x * x + y * y) / l);
    }
    std::sort(rim.begin(), rim.end());
    for (int b = 0; b < kSunBins; ++b) {
        if (rim.empty()) { s.rim[b] = s.size; continue; }
        const float a = -3.14159265f + (float(b) + 0.5f) * 6.2831853f / float(kSunBins);
        size_t k = 0;
        while (k < rim.size() && rim[k].first < a) ++k;
        const auto& hi = k < rim.size() ? rim[k] : rim.front();
        const auto& lo = k > 0 ? rim[k - 1] : rim.back();
        float span = hi.first - lo.first, at = a - lo.first;
        if (span <= 0.0f) span += 6.2831853f;
        if (at < 0.0f) at += 6.2831853f;
        const float f = span > 1e-5f ? std::clamp(at / span, 0.0f, 1.0f) : 0.0f;
        s.rim[b] = lo.second + (hi.second - lo.second) * f;
    }
    m_sunSprites.push_back(s);
}

// A sprite's outline packed: kSunBins reaches over 'reach' (8 bits each, 4 to a word).
static void PackShape(const float* rim, float reach, uint32_t out[8])
{
    for (int w = 0; w < 8; ++w) {
        out[w] = 0;
        for (int b = 0; b < 4; ++b) {
            const float r = reach > 0.0f ? std::clamp(rim[w * 4 + b] / reach, 0.0f, 1.0f) : 1.0f;
            out[w] |= uint32_t(std::lround(r * 255.0f)) << (8 * b);
        }
    }
}

// The suns under the clouds: at the first of the game's sun sprites (the sky's start). Which suns: the sprites of the
// last frame that had any (a few frames back at most), grouped by where they are (within ~6 degrees: one sun; its
// biggest sprite its glow's outline, its smallest its core's); else one, the sun's light's way, white, an even star.
void Device::DrawSun()
{
    if (m_sunDrawnFrame == m_frameNumber)
        return;
    m_sunDrawnFrame = m_frameNumber;
    m_sunsDrawn.clear();
    if (m_sun <= 0.0f || !m_sunPipeline)
        return;
    struct Sun { float dir[3], colour[3], weight; const SunSprite *big, *small; };
    Sun suns[4];
    int count = 0;
    if (m_sunSpritesFrame + 3 >= m_frameNumber)
        for (const SunSprite& s : m_sunSprites) {
            const float lum = 0.3f * s.colour[0] + 0.59f * s.colour[1] + 0.11f * s.colour[2] + 1e-3f;
            const float wgt = lum * std::max(s.size, 1e-4f);
            int k = 0;
            for (; k < count; ++k) {             // (its sum of directions, weighted: compared normalised)
                const float* d = suns[k].dir;
                const float dot = d[0] * s.dir[0] + d[1] * s.dir[1] + d[2] * s.dir[2];
                if (dot > 0.9945f * std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2])) break;
            }
            if (k == count) {
                if (count == 4) continue;
                suns[count++] = Sun{{0, 0, 0}, {0, 0, 0}, 0.0f, &s, &s};
            }
            Sun& u = suns[k];
            for (int j = 0; j < 3; ++j) {
                u.dir[j] += s.dir[j] * wgt;
                u.colour[j] += s.colour[j];
            }
            u.weight += wgt;
            if (s.size > u.big->size) u.big = &s;
            if (s.size < u.small->size) u.small = &s;
        }
    static SunSprite even;                       // (no sprites: an even star, its spikes from a hash)
    if (count == 0) {
        const float* d = m_sunLuminance > 0.0f ? m_sunDir : m_trueSunDir;
        for (int b = 0; b < kSunBins; ++b)
            even.rim[b] = 0.12f * ((b & 1) ? 0.5f + 0.2f * float((b * 37 + 5) % 13) / 12.0f
                                           : 0.8f + 0.2f * float((b * 37 + 5) % 13) / 12.0f);
        even.size = 0.12f;
        suns[0] = Sun{{-d[0], -d[1], -d[2]}, {1.0f, 1.0f, 1.0f}, 1.0f, &even, &even};
        count = 1;
    }
    float brightest = 0.0f;
    for (int k = 0; k < count; ++k) {
        Sun& u = suns[k];
        const float l = std::sqrt(u.dir[0] * u.dir[0] + u.dir[1] * u.dir[1] + u.dir[2] * u.dir[2]);
        if (l > 1e-6f) for (float& x : u.dir) x /= l;
        brightest = std::max(brightest, 0.3f * u.colour[0] + 0.59f * u.colour[1] + 0.11f * u.colour[2]);
    }
    std::sort(suns, suns + count, [](const Sun& a, const Sun& b) { return a.weight > b.weight; });   // (the main sun first)
    for (int k = 0; k < count && k < 2; ++k) {
        const Sun& u = suns[k];
        const float* to = u.dir;
        if (to[1] < -0.03f)
            continue;                            // (under the horizon)
        SunDrawn d{};
        std::memcpy(d.dir, to, sizeof(d.dir));
        // Its colour: the sprites' hue at full brightness (white for the white sun, bluish purple for the other),
        // a dimmer sun dimmer; golden then red low, a third through the air at the horizon, gone just under it.
        const float mx = std::max({u.colour[0], u.colour[1], u.colour[2], 1e-3f});
        const float lum = 0.3f * u.colour[0] + 0.59f * u.colour[1] + 0.11f * u.colour[2];
        const float rel = brightest > 0.0f ? std::clamp(lum / brightest, 0.5f, 1.0f) : 1.0f;
        const float low = 1.0f - std::clamp(to[1] / 0.3f, 0.0f, 1.0f), low2 = low * low;
        const float tint[3] = {1.0f, 1.0f - 0.35f * low2, 1.0f - 0.65f * low2};
        const float through = (0.35f + 0.65f * std::clamp(to[1] / 0.3f, 0.0f, 1.0f)) *
                              std::clamp((to[1] + 0.03f) / 0.05f, 0.0f, 1.0f);
        for (int j = 0; j < 3; ++j) d.colour[j] = u.colour[j] / mx * tint[j] * through * rel * m_sun;
        // Its outlines: the big sprite's (the glow) and the small one's (the core), both over the glow's reach.
        const float scale = std::clamp(m_sunSize, 0.1f, 3.0f);
        float bigRim[kSunBins], smallRim[kSunBins], reach = 0.0f, coreReach = 0.0f;
        for (int b = 0; b < kSunBins; ++b) {
            bigRim[b] = u.big->rim[b] * scale;
            smallRim[b] = u.small->rim[b] * scale * (u.small == u.big ? 0.45f : 1.0f);
            reach = std::max(reach, bigRim[b]);
            coreReach = std::max(coreReach, smallRim[b]);
        }
        d.reach = std::clamp(reach, 0.005f, 0.6f);
        d.core = coreReach / d.reach;
        PackShape(bigRim, d.reach, d.glowShape);
        PackShape(smallRim, d.reach, d.coreShape);
        m_sunsDrawn.push_back(d);
    }
    DrawSuns(false);
    if (m_frameNumber % 600 == 50) {
        Log("suns: %d from %zu sprites (last frame's), %zu drawn", count, m_sunSprites.size(), m_sunsDrawn.size());
        for (const SunSprite& s : m_sunSprites) {
            char rim[kSunBins * 4 + 1] = {}, *at = rim;   // (its outline: each reach over its longest, %)
            for (int b = 0; b < kSunBins; ++b)
                at += std::snprintf(at, 5, "%3d ", int(std::lround(s.rim[b] / std::max(s.size, 1e-6f) * 99.0f)));
            Log("  sprite: hub %u towards (%.3f %.3f %.3f) size %.3f colour %.2f %.2f %.2f outline %s", s.hub,
                double(s.dir[0]), double(s.dir[1]), double(s.dir[2]), double(s.size), double(s.colour[0]),
                double(s.colour[1]), double(s.colour[2]), rim);
        }
        for (const SunDrawn& d : m_sunsDrawn)
            Log("  sun: towards (%.3f %.3f %.3f) reach %.3f core %.2f colour %.2f %.2f %.2f", double(d.dir[0]),
                double(d.dir[1]), double(d.dir[2]), double(d.reach), double(d.core), double(d.colour[0]),
                double(d.colour[1]), double(d.colour[2]));
    }
}

// Their glow over the clouds (RVK_SunBleed): at the sky's end - after the game's sheets and our cloud layer, before
// the land (which covers it).
void Device::DrawSunOver()
{
    if (m_sunOverFrame == m_frameNumber || m_sunDrawnFrame != m_frameNumber)
        return;
    m_sunOverFrame = m_frameNumber;
    if (m_sunBleed > 0.0f)
        DrawSuns(true);
}

void Device::DrawSuns(bool over)
{
    if (m_sunsDrawn.empty() || !m_rendering || m_target != m_scene || !m_glow || !m_localFraction || !m_motionVectors ||
        !m_albedo || m_external)
        return;
    struct Push {
        d3d::Matrix invViewProj;
        float toSun[4], colour[4], view[4], anim[4];
        uint32_t glowShape[8], coreShape[8];
    } push{};
    static_assert(sizeof(Push) == 192, "sun.frag's push constants");
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sunPipeline);
    const float bleed = std::clamp(m_sunBleed, 0.0f, 1.0f);
    for (size_t k = 0; k < m_sunsDrawn.size(); ++k) {
        const SunDrawn& d = m_sunsDrawn[k];
        std::memcpy(push.toSun, d.dir, sizeof(d.dir));
        push.toSun[3] = d.reach;
        std::memcpy(push.colour, d.colour, sizeof(d.colour));
        push.colour[3] = d.core;
        push.view[0] = float(m_scene->m_width);
        push.view[1] = float(m_scene->m_height);
        push.view[2] = over ? 1.0f : 0.0f;       // under the clouds: all but the glow's bleed; over: the bleed alone
        push.view[3] = bleed;
        push.anim[0] = float(std::fmod(m_frameClock, 3600.0));   // the clock (spikes swelling, beams turning)
        push.anim[1] = float(k) * 17.31f;        // each sun its own
        std::memcpy(push.glowShape, d.glowShape, sizeof(push.glowShape));
        std::memcpy(push.coreShape, d.coreShape, sizeof(push.coreShape));
        vkCmdPushConstants(cmd, m_sunLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
}

}  // namespace rvk
