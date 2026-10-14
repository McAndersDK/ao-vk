// The shared wind (docs/storm-plan.md phase 1): one wind for everything that sways, falls or drifts - the ground grass,
// the plants and trees, the leaves, the water - read through Device::Wind (direction, its clock, strength) and Gusts().
// Once a frame (BeginFrame):
// - Where it blows: the game's weather wind while it hands one over (its rain, VisualRain_t::process), else the sky's
//   (SkyWind: turning slowly over hours); the direction turns towards it over some seconds, never snaps.
// - How hard: calm on a fair day, harder under cloud, and in a storm (the rain, eased in over a few seconds) up to about
//   three times as hard (RVK_WindStorm scales the storm's part), with gust fronts several times as strong.
// - Its clock (m_windTime): every wave and gust front moves on it. It runs faster in a storm - accumulated, so a change
//   of pace never makes the plants jump.
#include "internal.h"

namespace rvk {

namespace {
constexpr float kTurnSeconds = 8.0f;             // the direction's easing (a wind veers, it doesn't snap)
constexpr float kStormSeconds = 4.0f;            // the storm building up and dying down
}  // namespace

void Device::UpdateWind()
{
    const double now = m_frameClock;
    const float dt = m_windClockPrev < 0.0 ? 0.0f : float(std::clamp(now - m_windClockPrev, 0.0, 0.25));
    const bool paused = m_windClockPrev >= 0.0 && now - m_windClockPrev > 0.25;
    m_windClockPrev = now;

    // Where it blows.
    float target[2];
    const bool fromGame = now - m_weatherGameTime < 0.5;
    const float gx = m_weatherGame.wind[0], gz = m_weatherGame.wind[2];
    const float gl = std::hypot(gx, gz);
    if (fromGame && gl > 0.05f) {
        target[0] = gx / gl;
        target[1] = gz / gl;
    } else {
        SkyWind(target);
    }
    const float turn = dt > 0.0f ? 1.0f - std::exp(-dt / kTurnSeconds) : 1.0f;
    float x = m_windDir[0] + (target[0] - m_windDir[0]) * turn, z = m_windDir[1] + (target[1] - m_windDir[1]) * turn;
    const float l = std::hypot(x, z);
    if (l > 1e-4f) {
        m_windDir[0] = x / l;
        m_windDir[1] = z / l;
    }

    // How hard: the storm is the rain (rain.cpp eases it towards the game's weather), eased again.
    const float stormTarget = std::clamp(m_rain, 0.0f, 1.0f);
    const float ease = dt > 0.0f ? 1.0f - std::exp(-dt / kStormSeconds) : 1.0f;
    m_storm += (stormTarget - m_storm) * ease;
    if (m_storm < 1e-3f && stormTarget == 0.0f) m_storm = 0.0f;
    const float storm = m_storm * std::max(m_windStormScale, 0.0f);
    const float cloud = m_cloudParams.enable ? 0.25f * SkyCloudiness() : 0.0f;
    m_windStrength = 1.0f + cloud + 1.8f * storm;
    m_windGusts = 1.0f + 2.5f * storm;

    // Its clock: faster in a storm.
    m_windTimePrev = m_windTime;
    m_windTime = std::fmod(m_windTime + double(dt) * (1.0 + 0.9 * double(storm)), 3600.0);
    if (paused || m_windTime < m_windTimePrev)
        m_windTimePrev = m_windTime;             // wrapped, or the first frame after a pause (no motion from it)
    if (m_frameNumber % 600 == 0)
    {
        Log("wind: towards %.2f %.2f (%s), strength x%.2f, gusts x%.2f, storm %.2f; the game's storm tint left out %u "
            "times", double(m_windDir[0]), double(m_windDir[1]), fromGame && gl > 0.05f ? "the game's weather" : "the sky's",
            double(m_windStrength), double(m_windGusts), double(m_storm), m_stormTintSkipped);
        m_stormTintSkipped = 0;
    }
}

}  // namespace rvk
