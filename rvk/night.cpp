// Nights (docs/night-plan.md): the moon's light. Once the sun is down the game lights its scene with one dim blue
// directional light pointing up - the sun under the ground - and a grey ambient; the land gets no light from any
// side. Here the moon in the game's sky becomes a light: a dim cool one from where it stands, the shadow map drawn
// from it (shadow.cpp RenderShadowMap), added to the lit draws (draw.cpp) as the game's night light fades, and on
// the ground and grass, which the game keeps directional light off, added (FrameLights moon).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {

void Normalize(float v[3])
{
    float l = std::sqrt(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 0.0f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

float Dot(const float a[3], const float b[3]) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; }

float Smooth(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

// The moon's light at full strength (RVK_MoonLight 1): cool, silvery, dim - on flat ground under a moon ~27 degrees
// up about half the game's night ambient again.
constexpr float kMoon[3] = {0.20f, 0.25f, 0.36f};

constexpr uint32_t kMoonTexture = 16807;     // the big moon's texture (RDB type 1010004)

}  // namespace

// A mesh in the sky (SetDrawVisual: the sky's GenericMeshObject_t). The game's moons are lit 49-vertex discs (7 x 7),
// alpha blended, each drawn again additively over itself (its glow: skipped here); there are up to three, crossing the
// sky as the night goes on (user dump rvk-frame-143150: the big one setting behind the ruins). (The lit 584-vertex
// meshes, four fixed round the eye with one texture, are not moons.) The biggest seen this frame (by its size in the
// sky) is the moon that lights.
void Device::NoteSkyMesh(uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    if (!vertices || vertexCount != 49 || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ || !m_rs[d3d::RS_LIGHTING] ||
        !m_rs[d3d::RS_ALPHABLENDENABLE] || m_rs[d3d::RS_SRCBLEND] != d3d::BLEND_SRCALPHA ||
        m_rs[d3d::RS_DESTBLEND] != d3d::BLEND_INVSRCALPHA)
        return;
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        for (int k = 0; k < 3; ++k) {
            lo[k] = std::min(lo[k], p[k]);
            hi[k] = std::max(hi[k], p[k]);
        }
    }
    const auto& w = m_world.m;
    const auto& v = m_view.m;
    float centre[3], eye[3], d[3];
    for (int j = 0; j < 3; ++j) {
        const float c[3] = {0.5f * (lo[0] + hi[0]), 0.5f * (lo[1] + hi[1]), 0.5f * (lo[2] + hi[2])};
        centre[j] = c[0] * w[0][j] + c[1] * w[1][j] + c[2] * w[2][j] + w[3][j];
        eye[j] = -(v[3][0] * v[j][0] + v[3][1] * v[j][1] + v[3][2] * v[j][2]);
        d[j] = centre[j] - eye[j];
    }
    const float dist = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
    const float scale = std::sqrt(w[0][0] * w[0][0] + w[0][1] * w[0][1] + w[0][2] * w[0][2]);
    const float radius = 0.5f * std::max({hi[0] - lo[0], hi[1] - lo[1], hi[2] - lo[2]}) * scale;
    if (dist < 1.0f)
        return;
    const float size = radius / dist;
    // Which is which (tuning): every one, now and then.
    if ((m_frameNumber % 600) == 20) {
        const Texture* t = m_textures[0];
        Log("moon: a moon towards (%.2f %.2f %.2f), %.0f away, radius %.0f (size %.3f), texture rdb %u %ux%u, at (%.0f %.0f %.0f) "
            "from the eye",
            d[0] / dist, d[1] / dist, d[2] / dist, dist, radius, size, t ? t->m_rdbId : 0u, t ? t->Width() : 0u,
            t ? t->Height() : 0u, w[3][0] - eye[0], w[3][1] - eye[1], w[3][2] - eye[2]);
    }
    // The big moon's texture (RDB 1010004:16807, user) first, then the biggest.
    const Texture* tex = m_textures[0];
    const float rank = size + (tex && tex->m_rdbId == kMoonTexture ? 10.0f : 0.0f);
    if (m_moonSeenFrame == m_frameNumber && rank <= m_moonSeenSize)
        return;
    m_moonSeenFrame = m_frameNumber;
    m_moonSeenSize = rank;
    for (int j = 0; j < 3; ++j) m_moonSeenDir[j] = d[j] / dist;
}

// Once a frame, before the shadow map: where the moon is, how much it lights.
void Device::UpdateMoon()
{
    const bool wasOn = MoonOn();
    const bool sky = m_skySeenFrame != ~0ull && m_skySeenFrame + 30 >= m_frameNumber;
    float target[3];
    bool have = false;
    if (m_moonSeenFrame != ~0ull && m_moonSeenFrame + 30 >= m_frameNumber) {
        std::memcpy(target, m_moonSeenDir, sizeof(target));
        have = true;
    } else if (sky && m_sunLuminance > 0.0f) {
        // No moon seen (a zone without, or none drawn): opposite the sun, at least 25 degrees up.
        for (int j = 0; j < 3; ++j) target[j] = m_sunDir[j];
        target[1] = std::max(target[1], 0.42f);
        Normalize(target);
        have = true;
    }
    if (have) {
        if (Dot(target, m_moonPos) < 0.9f) {
            std::memcpy(m_moonPos, target, sizeof(target));     // (a new zone, the first sight: no swing across)
        } else {
            for (int j = 0; j < 3; ++j) m_moonPos[j] += (target[j] - m_moonPos[j]) * 0.05f;
            Normalize(m_moonPos);
        }
    }
    // Night by the sun's height: none with it 3 degrees up (where its shadows end), full 6 degrees down. The moon's
    // light by its own height: none at the horizon, full from ~27 degrees.
    const float sunUp = m_sunLuminance > 0.0f ? -m_sunDir[1] : -1.0f;
    const float night = 1.0f - Smooth(-0.10f, 0.05f, sunUp);
    const float target2 = have && m_moonLight > 0.0f ? night * Smooth(0.05f, 0.45f, m_moonPos[1]) : 0.0f;
    if (m_moonFade > 0.0f || target2 > 0.0f)
        m_constantsDirty = true;                 // (the draws' lights: MoonLightFor)
    m_moonFade += (target2 - m_moonFade) * 0.05f;
    if (m_moonFade < 0.002f && target2 == 0.0f) m_moonFade = 0.0f;
    if ((m_frameNumber % 600) == 30 || wasOn != MoonOn())
        Log("moon: %s, towards (%.2f %.2f %.2f), night %.2f, light %.2f (sun up %.2f, sky %s)",
            !have ? "none" : m_moonSeenFrame != ~0ull && m_moonSeenFrame + 30 >= m_frameNumber ? "seen" : "opposite the sun",
            m_moonPos[0], m_moonPos[1], m_moonPos[2], night, m_moonFade, sunUp, sky ? "seen" : "not seen");
    for (int j = 0; j < 3; ++j) {
        m_moonDir[j] = -m_moonPos[j];
        m_moonColour[j] = kMoon[j] * m_moonLight * m_moonFade;
    }
}

// The game's night light (pointing up: the sun under the ground) while the moon lights: kept as the game has it,
// fading out as the moon's light comes in (none with a high moon at night) - turned to come from the moon instead, it
// lit everything facing even a low moon, brighter than the game's night (user, 2026-10-13). false: not that light.
bool Device::MoonScaledGameLight(const d3d::Light& game, d3d::Light* out) const
{
    if (!MoonOn() || game.direction.y <= 0.0f)
        return false;                            // the sun, still up
    const float keep = std::max(0.0f, 1.0f - 4.0f * m_moonFade);
    *out = game;
    out->diffuse = {game.diffuse.r * keep, game.diffuse.g * keep, game.diffuse.b * keep, game.diffuse.a};
    out->specular = {game.specular.r * keep, game.specular.g * keep, game.specular.b * keep, game.specular.a};
    return true;
}

// The moon as a directional light (MoonOn).
d3d::Light Device::MoonAsLight() const
{
    d3d::Light l{};
    l.type = d3d::LIGHT_DIRECTIONAL;
    l.direction = {m_moonDir[0], m_moonDir[1], m_moonDir[2]};
    l.diffuse = {m_moonColour[0], m_moonColour[1], m_moonColour[2], 1.0f};
    l.specular = l.diffuse;
    return l;
}

}  // namespace rvk
