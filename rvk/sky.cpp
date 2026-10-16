// Sky weather (docs/sky-weather-plan.md): how cloudy the sky is, clear .. overcast, from the clock. The game draws the
// same flat sheets in every sky and says nothing of the weather but its rain, so the sky's own state comes from here:
// smooth noise over time - one sky lasting half an hour to an hour, changing over minutes - from the wall
// clock (UTC: AO's time runs from real time, so the same moment has the same sky on every login). Each zone leans it
// by its climate (phase 3): deserts and wastelands clearer, coasts, islands and forests cloudier (RVK_SkyZones).
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>

namespace rvk {

namespace {

// A knot's value, 0..1 (its index hashed).
float Knot(int64_t i, uint32_t salt)
{
    uint64_t x = uint64_t(i) * 0x9E3779B97F4A7C15ull ^ (uint64_t(salt) << 32);
    x ^= x >> 31; x *= 0xBF58476D1CE4E5B9ull; x ^= x >> 27; x *= 0x94D049BB133111EBull; x ^= x >> 31;
    return float(x >> 40) / 16777216.0f;
}

// Smooth value noise over time: knots `period` seconds apart, eased between.
float Noise(double t, double period, uint32_t salt)
{
    const double u = t / period;
    const int64_t i = int64_t(std::floor(u));
    const float f = float(u - double(i)), s = f * f * (3.0f - 2.0f * f);
    return Knot(i, salt) + (Knot(i + 1, salt) - Knot(i, salt)) * s;
}

// Rubi-Ka's outdoor playfields by climate (resource ids, rdb 1000001 names): how far their skies lean, in the noise's
// units before the mapping (0.6 of them span clear .. overcast) - a desert's sky is clear about half the time,
// overcast seldom; a coast's broken or overcast about half. Others (cities, the Shadowlands' own skies): 0.
struct ZoneSky { uint32_t playfield; float lean; };
constexpr ZoneSky kZoneSkies[] = {
    // deserts and wastelands
    {565, -0.20f},   // Newland Desert
    {566, -0.12f},   // Newland City
    {567, -0.12f},   // Newland
    {551, -0.18f},   // Wailing Wastes
    {560, -0.20f},   // Mort
    {570, -0.20f},   // The Perpetual Wastelands
    {760, -0.15f},   // 4 Holes
    {791, -0.15f},   // Holes in the Wall
    {795, -0.15f},   // The Longest Road
    {670, -0.10f},   // Clondyke
    {615, -0.08f},   // Southern Fouls Hills
    {620, -0.08f},   // Eastern Fouls Plains
    {6011, -0.18f},  // Arid Rift
    {6012, -0.12f},  // Neretva Canyons
    {6550, -0.10f},  // Uturn Canyon
    // coasts, islands, the sea
    {556, 0.15f},    // Coast of Peace
    {656, 0.15f},    // Coast of Tranquility
    {665, 0.15f},    // Broken Shores
    {585, 0.12f},    // Aegean
    {500, 0.10f},    // Parnassos
    {501, 0.10f},    // Parnassos Island of Ceremony
    {6010, 0.12f},   // Serenity Islands
    {4894, 0.12f},   // Coast of Harmony
    // forests, green and wet country, the river
    {505, 0.12f},    // Avalon
    {600, 0.08f},    // Varmint Woods
    {605, 0.10f},    // Belial Forest
    {716, 0.10f},    // Omni Forest
    {717, 0.10f},    // Greater Omni Forest
    {685, 0.10f},    // Galway County
    {687, 0.10f},    // Galway Shire
    {695, 0.06f},    // Lush Fields
    {630, 0.04f},    // Pleasant Meadows
    {646, 0.06f},    // Tir County
    {647, 0.06f},    // Greater Tir County
    {625, 0.08f},    // Milky Way
    {635, 0.05f},    // Stret East Bank
    {650, 0.05f},    // Upper Stret East Bank
    {790, 0.05f},    // Stret West Bank
    {6551, 0.08f},   // Uturn Forest
};

}  // namespace

void Device::SetSkyPlayfield(uint32_t playfield)
{
    if (playfield == m_skyPlayfield)
        return;
    m_skyPlayfield = playfield;
    m_skyZoneLean = 0.0f;
    for (const ZoneSky& z : kZoneSkies)
        if (z.playfield == playfield)
            m_skyZoneLean = z.lean;
    if (m_skyZoneLean != 0.0f)
        Log("sky: playfield %u leans its skies %s (%+.2f)", playfield, m_skyZoneLean < 0.0f ? "clearer" : "cloudier",
            double(m_skyZoneLean));
}

// Seconds on the sky's clock: the wall clock (UTC), or a test's (SetSkyTime).
double Device::SkyClock() const
{
    if (m_skyTime >= 0.0)
        return m_skyTime;
    using namespace std::chrono;
    return duration<double>(system_clock::now().time_since_epoch()).count();
}

// How cloudy the sky is: 0 clear .. 1 overcast. Knots 90 minutes apart, a lesser wander over 25: a sky lasts about half
// an hour (fair, scattered, broken) to an hour (clear); clear about a fifth of the time, overcast a seventh.
float Device::SkyCloudiness() const
{
    const double t = SkyClock();
    const float v = 0.75f * Noise(t, 5400.0, 17u) + 0.25f * Noise(t, 1500.0, 29u) +
                    m_skyZoneLean * std::clamp(m_cloudParams.zones, 0.0f, 1.0f);
    return std::clamp((v - 0.25f) / 0.6f, 0.0f, 1.0f);
}

// The wind's way when the game gives none (its rain's wind gives one): turning slowly over hours, from the clock -
// x, z unit vector.
void Device::SkyWind(float out[2]) const
{
    const double t = SkyClock();
    const float a = 6.2831853f * (Noise(t, 14400.0, 41u) * 1.6f + Noise(t, 3600.0, 53u) * 0.4f);
    out[0] = std::cos(a);
    out[1] = std::sin(a);
}

// The sky's state by name (the log).
const char* Device::SkyName(float c)
{
    return c < 0.08f ? "clear" : c < 0.3f ? "fair" : c < 0.55f ? "scattered" : c < 0.8f ? "broken" : "overcast";
}

}  // namespace rvk
