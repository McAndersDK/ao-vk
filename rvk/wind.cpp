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
        // (Eased, as its strength: in the game's sandstorm its colour flickers frame to frame - our air, its light
        // and the land's tint with it, the user saw the light flicker.)
        const float colourEase = dt > 0.0f ? 1.0f - std::exp(-dt / 1.5f) : 1.0f;
        for (int i = 0; i < 3; ++i)
            m_sandColour[i] += (1.3f * (luma + (rgb[i] - luma) * 0.5f) - m_sandColour[i]) * (m_sand < 1e-3f ? 1.0f : colourEase);
        // (How much the game's tint swings: logged.)
        static uint32_t lo[4] = {255, 255, 255, 255}, hi[4] = {0, 0, 0, 0}, frames = 0;
        for (int i = 0; i < 4; ++i) {
            const uint32_t c = (tintColour >> (8 * (3 - i))) & 0xFFu;
            lo[i] = std::min(lo[i], c);
            hi[i] = std::max(hi[i], c);
        }
        if (++frames == 600) {
            Log("storm tint (game's, last 600 frames): a %u..%u r %u..%u g %u..%u b %u..%u\n", lo[0], hi[0], lo[1], hi[1],
                lo[2], hi[2], lo[3], hi[3]);
            frames = 0;
            for (int i = 0; i < 4; ++i) { lo[i] = 255; hi[i] = 0; }
        }
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
    // A sandstorm takes this much of the sun's direct light and spreads it (lighting.glsl): the game's full storm
    // (0.38) ~0.6 at RVK_SandHaze 1; never all of it. With the sand fog: what its dust column (sand_fog.frag: 0.7 x
    // 120 + 0.3 x 1500 units deep, the sun's way through it eased to kSunPath of it) takes on the sun's way down -
    // the same as from the sun's disc behind the fog (w: 1, reddened alike).
    if (SandFogOn()) {
        const float kSunPath = 0.3f, column = 0.7f * 120.0f + 0.3f * 1500.0f;
        const float up = std::max(-m_frameSunDir[1], 0.05f);
        out[2] = std::clamp(1.0f - std::exp(-kSunPath * 0.075f * m_sand * m_sandHaze * column / up), 0.0f, 0.97f);
        out[3] = 1.0f;
    } else {
        out[2] = std::clamp(m_sand * m_sandHaze * 1.6f, 0.0f, 0.75f);
        out[3] = 0.0f;
    }
}

void Device::WindAxes(float x, float z, float out[2]) const
{
    const double dx = x - m_windPivot[0], dz = z - m_windPivot[1];
    out[0] = float(dx * m_windDir[0] + dz * m_windDir[1] + m_windOffset[0]);
    out[1] = float(-dx * m_windDir[1] + dz * m_windDir[0] + m_windOffset[1]);
}

// The tone mapping's distant gusts (occlusion.glsl gust0..3): the camera (the scene's view and projection, as its
// depth), where the simulated sand ends, the wind's frame carried with it.
void Device::SandGusts(float out[16]) const
{
    for (int i = 0; i < 16; ++i) out[i] = 0.0f;
    if (m_sand <= 0.0f || m_sandHaze <= 0.0f || m_sandGusts <= 0.0f || !m_aoProjValid)
        return;
    const auto& v = m_aoView.m;
    for (int i = 0; i < 3; ++i) out[i] = -(v[3][0] * v[i][0] + v[3][1] * v[i][1] + v[3][2] * v[i][2]);
    float ground;
    out[3] = GroundAt(out[0], out[2], &ground, nullptr, nullptr) ? ground : out[1] - 2.0f;
    for (int i = 0; i < 3; ++i) out[4 + i] = v[i][2];   // (the view's forward in the world: its third column)
    out[7] = m_aoProj.m[0][0];
    out[8] = m_aoProj.m[1][1];
    out[9] = 0.45f * m_sandBoxSize;              // (the simulated sand thins out from here: smoke.frag's edge band)
    out[10] = m_sandGusts;
    out[11] = float(std::fmod(m_windTime, 3600.0));
    float frame[4];
    WindFrame(frame);
    out[12] = m_windDir[0];
    out[13] = m_windDir[1];
    out[14] = frame[0] - float(m_sandFarCarried);
    out[15] = frame[1];
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
    out[6] *= 1.0f - 0.75f * m_sandEyeShelter;               // (in a hill's or a wall's lee: the air near clears)
    out[7] = (SmokeCoverValid() ? 1.0f : 0.0f) + (m_sandFogValid ? 2.0f : 0.0f);   // (the fog: RenderSandFog)
    // (With the fog, z: how much the light on the land takes the dust's colour - the sun's light the storm takes on
    // its way down comes to the land scattered by the dust, its colour - x RVK_SandTint.)
    if (m_sandFogValid) {
        float frame[4];
        WindFrame(frame);
        out[6] = frame[2] * std::clamp(m_sandTint, 0.0f, 1.0f);
    }
}

}  // namespace rvk
