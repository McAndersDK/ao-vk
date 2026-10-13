// The sun's disc (RVK_Sun; sun.frag). The game's sun is only four additive ray sprites (VisualSunRays_t) - no disc -
// and our cloud sheets and layer, drawn after them, left little of it. Drawn where the game draws its first sprite:
// into the sky before its sheets and the cloud layer, which cover it as thick as they are. About twice the real sun's
// size, very bright (the bloom makes its glare), golden then red and dimmer low in the sky (its light through the
// air, as tonemap.frag Air's glow), a halo and a faint starburst. The game's sprites stay (user's choices, 2026-10-13).
#include "internal.h"

#include <algorithm>
#include <cmath>

namespace rvk {

using namespace vk;
using namespace detail;

void Device::DrawSun()
{
    if (m_sunDrawnFrame == m_frameNumber)
        return;
    m_sunDrawnFrame = m_frameNumber;
    if (m_sun <= 0.0f || !m_sunPipeline || !m_rendering || m_target != m_scene || !m_glow || !m_localFraction ||
        !m_motionVectors || !m_albedo || m_external)
        return;
    // Towards the sun: this frame's (the sprites are drawn with its light set), else the last seen.
    const float* d = m_sunLuminance > 0.0f ? m_sunDir : m_trueSunDir;
    float to[3] = {-d[0], -d[1], -d[2]};
    const float tl = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (tl < 1e-6f)
        return;
    for (float& v : to) v /= tl;
    if (to[1] < -0.03f)
        return;                                  // (under the horizon)
    struct Push {
        d3d::Matrix invViewProj;
        float toSun[4], colour[4], view[4];
    } push{};
    if (!InvertMatrix(MulMatrix(m_view, m_proj), &push.invViewProj))
        return;
    // Its light: the sun's colour (white if none set), golden then red low, a third through the air at the horizon,
    // gone just under it; ~25x white (the bloom: a glaring disc).
    float sun[3] = {m_sunColor[0], m_sunColor[1], m_sunColor[2]};
    if (sun[0] + sun[1] + sun[2] <= 0.0f) sun[0] = sun[1] = sun[2] = 1.0f;
    const float low = 1.0f - std::clamp(to[1] / 0.3f, 0.0f, 1.0f), low2 = low * low;
    const float tint[3] = {1.0f, 1.0f - 0.35f * low2, 1.0f - 0.65f * low2};
    const float through = (0.35f + 0.65f * std::clamp(to[1] / 0.3f, 0.0f, 1.0f)) *
                          std::clamp((to[1] + 0.03f) / 0.05f, 0.0f, 1.0f);
    for (int k = 0; k < 3; ++k) {
        push.toSun[k] = to[k];
        push.colour[k] = sun[k] * tint[k] * through * 25.0f * m_sun;
    }
    push.toSun[3] = 0.0093f;                     // (radians: about twice the real sun's 0.27 degrees)
    push.colour[3] = 1.0f;                       // the halo
    push.view[0] = float(m_scene->m_width);
    push.view[1] = float(m_scene->m_height);
    push.view[2] = 0.5f;                         // the starburst
    push.view[3] = 0.09f;                        // the halo's width (radians, ~5 degrees)
    FlushGroup();
    VkCommandBuffer cmd = m_frames[m_frameIndex].main;
    const VkViewport full{0.0f, 0.0f, float(m_scene->m_width), float(m_scene->m_height), 0.0f, 1.0f};
    const VkRect2D area{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &full);
    vkCmdSetScissor(cmd, 0, 1, &area);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_sunPipeline);
    vkCmdPushConstants(cmd, m_sunLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    m_cache = StateCache{};                      // (not the scene's pipeline: its state set again)
    m_arenaBound = m_bindlessBound = false;
}

}  // namespace rvk
