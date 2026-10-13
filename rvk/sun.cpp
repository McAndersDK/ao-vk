// The suns (RVK_Sun; sun.frag). Rubi-Ka has two suns, and the game shows each as two additive sprites (VisualSunRays_t:
// four a frame, round fans of 32 triangles with one texture, newsun_frame01.png - a white-hot core with a serrated
// edge, a pinkish glow, faint rays -, tinted, one orange and one blue a sun, of very different sizes: added, the
// stock's big jagged white glare with warm and cyan fringes). Our cloud sheets and layer, drawn after them, left
// little of them. Each frame the sprites are noted (where - their fan's hub -, how big, their tint); at the first
// sprite of the next frame each is drawn again into the sky, before the game's sheets and the cloud layer, which cover
// it as thick as they are; at the sky's end a share of its glow again, over the clouds (RVK_SunBleed). Drawn as AO's,
// not a real sun (user, 2026-10-13): the texture made anew in sun.frag - its serrations swelling, its rays
// shimmering and turning -, in HDR (its core glaring), a bit tamer than the game's (RVK_SunSize). The game's own
// sprites off by default (RVK_GameSun).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

// A sun sprite: a fan of triangles round its hub (the vertex most triangles share). Where (the hub through its
// matrix, from the eye), how big (its farthest vertex from the hub, over the distance), its tint (the hub's colour
// times its alpha: the sprite is blended SRCALPHA, ONE) and how its rim's tint compares (Gouraud across the fan).
void Device::NoteSunSprite(uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                           uint32_t indexCount)
{
    if (m_sunSpritesFrame != m_frameNumber) {    // (a new frame: the last one's kept until now - DrawSun read them)
        m_sunSprites.clear();
        m_sunSpritesFrame = m_frameNumber;
    }
    if (!vertices || vertexCount < 2 || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || m_sunSprites.size() >= 8)
        return;
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    uint32_t hub = 0;
    if (indices && indexCount > 0 && vertexCount <= 256) {
        uint16_t uses[256] = {};
        for (uint32_t i = 0; i < indexCount; ++i)
            if (indices[i] < vertexCount && ++uses[indices[i]] > uses[hub]) hub = indices[i];
    }
    auto tint = [&](uint32_t i, float out[3]) {   // (premultiplied by alpha)
        if (layout.offset[2] < 0) { out[0] = out[1] = out[2] = 1.0f; return; }
        uint32_t argb;
        std::memcpy(&argb, base + size_t(i) * layout.stride + layout.offset[2], 4);
        const float a = float(argb >> 24) / 255.0f;
        out[0] = float((argb >> 16) & 0xFF) / 255.0f * a;
        out[1] = float((argb >> 8) & 0xFF) / 255.0f * a;
        out[2] = float(argb & 0xFF) / 255.0f * a;
    };
    float c[3], col[3], rim[3] = {};
    std::memcpy(c, base + size_t(hub) * layout.stride + layout.offset[0], sizeof(c));
    tint(hub, col);
    float reach = 0.0f;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        if (i == hub)
            continue;
        float p[3], t[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        reach = std::max(reach, (p[0] - c[0]) * (p[0] - c[0]) + (p[1] - c[1]) * (p[1] - c[1]) + (p[2] - c[2]) * (p[2] - c[2]));
        tint(i, t);
        for (int k = 0; k < 3; ++k) rim[k] += t[k] / float(vertexCount - 1);
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
    std::memcpy(s.colour, col, sizeof(col));
    const float hubLum = col[0] + col[1] + col[2], rimLum = rim[0] + rim[1] + rim[2];
    s.rimFade = hubLum > 1e-4f ? std::clamp(rimLum / hubLum, 0.0f, 1.0f) : 1.0f;
    s.hub = hub;
    m_sunSprites.push_back(s);
}

// The sprites under the clouds: at the first of the game's sun sprites (the sky's start); those of the last frame that
// had any (a few frames back at most); else one, the sun's light's way, white.
void Device::DrawSun()
{
    if (m_sunDrawnFrame == m_frameNumber)
        return;
    m_sunDrawnFrame = m_frameNumber;
    m_sunsDrawn.clear();
    if (m_sun <= 0.0f || !m_sunPipeline)
        return;
    std::vector<SunSprite> sprites;
    if (m_sunSpritesFrame + 3 >= m_frameNumber)
        sprites = m_sunSprites;
    if (sprites.empty()) {
        const float* d = m_sunLuminance > 0.0f ? m_sunDir : m_trueSunDir;
        sprites.push_back(SunSprite{{-d[0], -d[1], -d[2]}, 0.25f, {1.0f, 1.0f, 1.0f}, 1.0f, 0});
    }
    const float scale = std::clamp(m_sunSize, 0.1f, 3.0f);
    for (size_t k = 0; k < sprites.size(); ++k) {
        const SunSprite& s = sprites[k];
        const float* to = s.dir;
        if (to[1] < -0.03f)
            continue;                            // (under the horizon)
        // Golden then red low in the sky, a third through the air at the horizon, gone just under it.
        const float low = 1.0f - std::clamp(to[1] / 0.3f, 0.0f, 1.0f), low2 = low * low;
        const float tint[3] = {1.0f, 1.0f - 0.35f * low2, 1.0f - 0.65f * low2};
        const float through = (0.35f + 0.65f * std::clamp(to[1] / 0.3f, 0.0f, 1.0f)) *
                              std::clamp((to[1] + 0.03f) / 0.05f, 0.0f, 1.0f);
        SunDrawn d{};
        std::memcpy(d.dir, to, sizeof(d.dir));
        d.size = std::clamp(s.size * scale, 0.002f, 1.2f);
        for (int j = 0; j < 3; ++j) d.colour[j] = s.colour[j] * tint[j] * through * m_sun;
        d.rimFade = s.rimFade;
        d.seed = float(k) * 17.31f;
        m_sunsDrawn.push_back(d);
    }
    DrawSuns(false);
    if (m_frameNumber % 600 == 50) {
        Log("suns: %zu sprites (last frame's), %zu drawn", m_sunSprites.size(), m_sunsDrawn.size());
        for (const SunSprite& s : m_sunSprites)
            Log("  sprite: hub %u towards (%.3f %.3f %.3f) size %.3f tint %.2f %.2f %.2f rim %.2f", s.hub,
                double(s.dir[0]), double(s.dir[1]), double(s.dir[2]), double(s.size), double(s.colour[0]),
                double(s.colour[1]), double(s.colour[2]), double(s.rimFade));
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
    } push{};
    static_assert(sizeof(Push) == 128, "sun.frag's push constants");
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sunPipeline);
    for (const SunDrawn& d : m_sunsDrawn) {
        std::memcpy(push.toSun, d.dir, sizeof(d.dir));
        push.toSun[3] = d.size;
        std::memcpy(push.colour, d.colour, sizeof(d.colour));
        push.colour[3] = d.rimFade;
        push.view[0] = float(m_scene->m_width);
        push.view[1] = float(m_scene->m_height);
        push.view[2] = over ? 1.0f : 0.0f;       // under the clouds: all but the glow's bleed; over: the bleed alone
        push.view[3] = std::clamp(m_sunBleed, 0.0f, 1.0f);
        push.anim[0] = float(std::fmod(m_frameClock, 3600.0));   // the clock (serrations swelling, rays turning)
        push.anim[1] = d.seed;                   // each sprite its own
        vkCmdPushConstants(cmd, m_sunLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
        vkCmdDraw(cmd, 3, 1, 0, 0);
    }
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
}

}  // namespace rvk
