// Sky weather (docs/sky-weather-plan.md): how cloudy the sky is, clear .. overcast, from the clock. The game draws the
// same flat sheets in every sky and says nothing of the weather but its rain, so the sky's own state comes from here:
// smooth noise over time - one sky lasting half an hour to an hour, changing over minutes - from the wall
// clock (UTC: AO's time runs from real time, so the same moment has the same sky on every login).
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

}  // namespace

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
    const float v = 0.75f * Noise(t, 5400.0, 17u) + 0.25f * Noise(t, 1500.0, 29u);
    return std::clamp((v - 0.25f) / 0.6f, 0.0f, 1.0f);
}

// The sky's state by name (the log).
const char* Device::SkyName(float c)
{
    return c < 0.08f ? "clear" : c < 0.3f ? "fair" : c < 0.55f ? "scattered" : c < 0.8f ? "broken" : "overcast";
}

}  // namespace rvk
