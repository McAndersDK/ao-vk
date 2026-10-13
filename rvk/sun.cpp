// The suns (RVK_Sun; sun.frag). Rubi-Ka has two suns, and the game shows each only as additive ray sprites
// (VisualSunRays_t: four a frame, tinted - a white sun, a bluish purple one) - no disc; our cloud sheets and layer,
// drawn after them, left little of them. Each frame the sprites are noted (where, how big, their colour) and grouped
// into suns; at the first sprite of the next frame each sun is drawn into the sky, before the game's sheets and the
// cloud layer, which cover it as thick as they are: a disc (about twice the real sun's size, very bright - the bloom
// its glare - limb darkened, golden then red and dimmer low in the sky), a halo, and its rays - the game's flare made
// anew: streaks of many lengths, turning slowly and shimmering, in the sun's colour. The game's own sprites off by
// default (RVK_GameSun). User's choices, 2026-10-13.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

// A sun sprite: where (its middle through its matrix, from the eye), how big (its farthest vertex from the middle,
// over the distance), its colour (the vertices' mean).
void Device::NoteSunSprite(uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    if (m_sunSpritesFrame != m_frameNumber) {    // (a new frame: the last one's kept until now - DrawSun read them)
        m_sunSprites.clear();
        m_sunSpritesFrame = m_frameNumber;
    }
    if (!vertices || vertexCount == 0 || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || m_sunSprites.size() >= 8)
        return;
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    float c[3] = {}, col[3] = {};
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint8_t* v = base + size_t(i) * layout.stride;
        float p[3];
        std::memcpy(p, v + layout.offset[0], sizeof(p));
        for (int k = 0; k < 3; ++k) c[k] += p[k] / float(vertexCount);
        if (layout.offset[2] >= 0) {
            uint32_t argb;
            std::memcpy(&argb, v + layout.offset[2], 4);
            col[0] += float((argb >> 16) & 0xFF) / 255.0f / float(vertexCount);
            col[1] += float((argb >> 8) & 0xFF) / 255.0f / float(vertexCount);
            col[2] += float(argb & 0xFF) / 255.0f / float(vertexCount);
        }
    }
    float reach = 0.0f;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        reach = std::max(reach, (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]));
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
    m_sunSprites.push_back(s);
}

void Device::DrawSun()
{
    if (m_sunDrawnFrame == m_frameNumber)
        return;
    m_sunDrawnFrame = m_frameNumber;
    if (m_sun <= 0.0f || !m_sunPipeline || !m_rendering || m_target != m_scene || !m_glow || !m_localFraction ||
        !m_motionVectors || !m_albedo || m_external)
        return;
    // The suns: the sprites of the last frame that had any (a few frames back at most), grouped by where they are
    // (within ~6 degrees: one sun); else one, the sun's light's way, white.
    struct Sun { float dir[3], size, colour[3], weight; };
    Sun suns[4];
    int count = 0;
    if (m_sunSpritesFrame + 3 >= m_frameNumber)
        for (const SunSprite& s : m_sunSprites) {
            const float lum = 0.3f * s.colour[0] + 0.59f * s.colour[1] + 0.11f * s.colour[2] + 1e-3f;
            const float wgt = lum * std::max(s.size, 1e-4f);
            int k = 0;
            for (; k < count; ++k)
                if (suns[k].dir[0] * s.dir[0] + suns[k].dir[1] * s.dir[1] + suns[k].dir[2] * s.dir[2] > 0.9945f) break;
            if (k == count) {
                if (count == 4) continue;
                suns[count++] = Sun{{0, 0, 0}, 0.0f, {0, 0, 0}, 0.0f};
            }
            Sun& u = suns[k];
            for (int j = 0; j < 3; ++j) {
                u.dir[j] += s.dir[j] * wgt;
                u.colour[j] += s.colour[j];
            }
            u.size = std::max(u.size, s.size);
            u.weight += wgt;
        }
    if (count == 0) {
        const float* d = m_sunLuminance > 0.0f ? m_sunDir : m_trueSunDir;
        suns[0] = Sun{{-d[0], -d[1], -d[2]}, 0.0f, {1.0f, 1.0f, 1.0f}, 1.0f};
        count = 1;
    }
    float biggest = 0.0f, brightest = 0.0f;
    for (int k = 0; k < count; ++k) {
        Sun& u = suns[k];
        const float l = std::sqrt(u.dir[0] * u.dir[0] + u.dir[1] * u.dir[1] + u.dir[2] * u.dir[2]);
        if (l > 1e-6f) for (float& x : u.dir) x /= l;
        biggest = std::max(biggest, u.size);
        brightest = std::max(brightest, 0.3f * u.colour[0] + 0.59f * u.colour[1] + 0.11f * u.colour[2]);
    }
    struct Push {
        d3d::Matrix invViewProj;
        float toSun[4], colour[4], view[4], rays[4];
    } push{};
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sunPipeline);
    for (int k = 0; k < count && k < 2; ++k) {
        const Sun& u = suns[k];
        const float* to = u.dir;
        if (to[1] < -0.03f)
            continue;                            // (under the horizon)
        // Its colour: the sprites' hue at full brightness (white for the white sun, bluish purple for the other),
        // a dimmer sun dimmer; golden then red low, a third through the air at the horizon, gone just under it;
        // ~25x white (the bloom: a glaring disc).
        const float mx = std::max({u.colour[0], u.colour[1], u.colour[2], 1e-3f});
        const float lum = 0.3f * u.colour[0] + 0.59f * u.colour[1] + 0.11f * u.colour[2];
        const float rel = brightest > 0.0f ? std::clamp(lum / brightest, 0.5f, 1.0f) : 1.0f;
        const float low = 1.0f - std::clamp(to[1] / 0.3f, 0.0f, 1.0f), low2 = low * low;
        const float tint[3] = {1.0f, 1.0f - 0.35f * low2, 1.0f - 0.65f * low2};
        const float through = (0.35f + 0.65f * std::clamp(to[1] / 0.3f, 0.0f, 1.0f)) *
                              std::clamp((to[1] + 0.03f) / 0.05f, 0.0f, 1.0f);
        for (int j = 0; j < 3; ++j) {
            push.toSun[j] = to[j];
            push.colour[j] = u.colour[j] / mx * tint[j] * through * rel * 25.0f * m_sun;
        }
        // Its size: about twice the real sun's (0.27 degrees), the smaller sun's sprites smaller.
        const float size = biggest > 0.0f && u.size > 0.0f ? std::clamp(u.size / biggest, 0.6f, 1.0f) : 1.0f;
        push.toSun[3] = 0.0093f * size;
        push.colour[3] = 1.0f;                   // the halo
        push.view[0] = float(m_scene->m_width);
        push.view[1] = float(m_scene->m_height);
        push.view[2] = 0.0f;                     // (the old starburst: the rays now)
        push.view[3] = 0.09f;                    // the halo's width (radians, ~5 degrees)
        push.rays[0] = float(std::fmod(m_frameClock, 3600.0));   // the rays' clock (turning, shimmering)
        push.rays[1] = 1.0f;                     // their strength
        push.rays[2] = float(k) * 17.31f;        // each sun its own rays
        push.rays[3] = 0.12f;                    // their reach (radians, ~7 degrees)
        vkCmdPushConstants(cmd, m_sunLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
    if (m_frameNumber % 600 == 50)
        Log("suns: %d from %zu sprites (last frame's): first towards (%.2f %.2f %.2f) colour %.2f %.2f %.2f size %.3f; "
            "second %s", count, m_sunSprites.size(), double(suns[0].dir[0]), double(suns[0].dir[1]),
            double(suns[0].dir[2]), double(suns[0].colour[0]), double(suns[0].colour[1]), double(suns[0].colour[2]),
            double(suns[0].size), count > 1 ? "seen" : "none");
}

}  // namespace rvk
