// The shared wind (docs/storm-plan.md phase 1): one wind for everything that sways, falls or drifts - the ground grass,
// the plants and trees, the leaves, the water - read through Device::Wind (direction, its clock, strength) and Gusts().
// Once a frame (BeginFrame):
// - Where it blows: the game's weather wind while it hands one over (its rain, VisualRain_t::process), else the sky's
//   (SkyWind: turning slowly over hours); the direction turns towards it over some seconds, never snaps.
// - How hard: calm on a fair day, harder under cloud, and in a storm (the rain, or a sand or ash storm's tint, eased in
//   over a few seconds) up to about three times as hard (RVK_WindStorm scales the storm's part), with gust fronts several times as strong.
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

    // A sand or ash storm: the game's tint, left out in Draw, says its colour and strength; eased in and out.
    // RVK_SandForce: one whatever the weather, as strong as the game's full one (alpha ~0.38) x it, in a sand colour
    // (the demo's stand-in for the game's) or an ash grey (RVK_SandAsh).
    const bool forced = m_sandForce > 0.0f;
    const bool tint = forced || now - m_stormTintSeen < 0.5;
    const uint32_t tintColour = forced ? (m_sandAsh ? 0x009A9590u : 0x00C07830u) : m_stormTintColour;
    const float sandTarget = forced ? 0.38f * m_sandForce : tint ? float(m_stormTintColour >> 24) / 255.0f : 0.0f;
    m_sand += (sandTarget - m_sand) * (dt > 0.0f ? 1.0f - std::exp(-dt / 3.0f) : 1.0f);
    if (m_sand < 1e-3f && sandTarget == 0.0f) m_sand = 0.0f;
    if (tint) {
        // Its colour, made linear, then half-way towards its own brightness and a little lighter: the game's sheet is
        // a third over a greyish frame - at full strength (the horizon) its raw colour is a saturated orange.
        auto linear = [](uint32_t v) { const float c = float(v & 0xFF) / 255.0f; return c * c; };   // (about sRGB)
        const float rgb[3] = {linear(tintColour >> 16), linear(tintColour >> 8), linear(tintColour)};
        const float luma = 0.3f * rgb[0] + 0.59f * rgb[1] + 0.11f * rgb[2];
        for (int i = 0; i < 3; ++i) m_sandColour[i] = 1.3f * (luma + (rgb[i] - luma) * 0.5f);
    }

    // How hard: the storm is the rain (rain.cpp eases it towards the game's weather) or the sand (the game's ~0.38 a full
    // sandstorm), eased again.
    const float stormTarget = std::clamp(std::max(m_rain, 2.5f * m_sand), 0.0f, 1.0f);
    const float ease = dt > 0.0f ? 1.0f - std::exp(-dt / kStormSeconds) : 1.0f;
    m_storm += (stormTarget - m_storm) * ease;
    if (m_storm < 1e-3f && stormTarget == 0.0f) m_storm = 0.0f;
    const float storm = m_storm * std::max(m_windStormScale, 0.0f);
    const float cloud = m_cloudParams.enable ? 0.25f * SkyCloudiness() : 0.0f;
    m_windStrength = 1.0f + cloud + 1.8f * storm;
    m_windGusts = 1.0f + 2.5f * storm;

    // The patterns' frame (WindFrame): its pivot kept near the camera.
    if (m_frameEyeValid) {
        const double ex = m_frameEye[0], ez = m_frameEye[2];
        if (!m_windPivotSet || std::hypot(ex - m_windPivot[0], ez - m_windPivot[1]) > 64.0) {
            if (m_windPivotSet) {               // (the same (along, across) everywhere from the new pivot)
                const double dx = ex - m_windPivot[0], dz = ez - m_windPivot[1];
                m_windOffset[0] += dx * m_windDir[0] + dz * m_windDir[1];
                m_windOffset[1] += -dx * m_windDir[1] + dz * m_windDir[0];
            }
            m_windPivot[0] = ex;
            m_windPivot[1] = ez;
            m_windPivotSet = true;
        }
    }

    // Its clock: faster in a storm.
    m_windTimePrev = m_windTime;
    // (At most 0.1 s a frame: a long frame - a zone loading - slows the plants down rather than jumping their sway by
    // most of a swing; a storm's quick sway freaked out.)
    m_windTime = std::fmod(m_windTime + double(std::min(dt, 0.1f)) * (1.0 + 0.9 * double(storm)), 3600.0);
    if (paused || m_windTime < m_windTimePrev)
        m_windTimePrev = m_windTime;             // wrapped, or the first frame after a pause (no motion from it)
    if (m_frameNumber % 600 == 0)
    {
        Log("wind: towards %.2f %.2f (%s), strength x%.2f, gusts x%.2f, storm %.2f; the game's storm tint left out %u "
            "times (colour %08x), sand %.2f%s", double(m_windDir[0]), double(m_windDir[1]),
            fromGame && gl > 0.05f ? "the game's weather" : "the sky's", double(m_windStrength), double(m_windGusts),
            double(m_storm), m_stormTintSkipped, m_stormTintColour, double(m_sand),
            forced ? (m_sandAsh ? " (forced: ash)" : " (forced: sand)") : "");
        m_stormTintSkipped = 0;
    }
}

void Device::WindFrame(float out[4]) const
{
    // (along, across) = (dot(xz, wd), dot(xz, wd turned left)) + these: the pivot and the offset folded together.
    const double ax = m_windDir[0], az = m_windDir[1];
    out[0] = float(m_windOffset[0] - (m_windPivot[0] * ax + m_windPivot[1] * az));
    out[1] = float(m_windOffset[1] - (-m_windPivot[0] * az + m_windPivot[1] * ax));
    out[2] = out[3] = 0.0f;
}

void Device::WindAxes(float x, float z, float out[2]) const
{
    const double dx = x - m_windPivot[0], dz = z - m_windPivot[1];
    out[0] = float(dx * m_windDir[0] + dz * m_windDir[1] + m_windOffset[0]);
    out[1] = float(-dx * m_windDir[1] + dz * m_windDir[0] + m_windOffset[1]);
}

// The tone mapping's sand (occlusion.glsl dust, dust2): the storm's colour; its density - the game's tint at its
// strength (alpha ~0.38 in its sandstorm) a 50% veil at ~25 units, x RVK_SandHaze; surging with the wind.
void Device::SandHaze(float out[8]) const
{
    for (int i = 0; i < 8; ++i) out[i] = 0.0f;
    if (m_sand <= 0.0f || m_sandHaze <= 0.0f) return;
    // (Lit as the land: at night 40% as bright - else a glowing orange sky over dark land.)
    const float light = 1.0f - 0.6f * NightAmount();
    for (int i = 0; i < 3; ++i) out[i] = m_sandColour[i] * light;
    out[3] = 0.075f * m_sand * m_sandHaze;
    out[4] = float(m_windTime);
    out[5] = 0.12f + 0.25f * std::min(m_storm + m_sand, 1.0f);
    out[6] = std::min(0.45f * m_sand * m_sandHaze, 0.6f);   // (the game's 0.38: a ~17% veil close by)
    out[7] = SmokeCoverValid() ? 1.0f : 0.0f;
}

}  // namespace rvk
