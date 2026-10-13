// Valley mist (RVK_Mist): a layer of mist lying in the low ground round the eye - valleys, hollows, over water - at
// night and round dawn, burning off as the sun climbs. Where: the low ground is measured from the terrain's own
// vertices as it is drawn (the lowest sixth within 800 units), the layer's top ~25 units over it, fading softly above.
// When: by the sun's height (most before and at sunrise, little in the evening - mist forms as the night cools, gone by
// late morning), the sky (clear, calm nights the most) and rain lately. Drawn in the tone mapping with the air
// (tonemap.frag Air: the layer's optical depth along each ray, integrated by height).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace detail;

namespace {

float Smooth(float a, float b, float x)
{
    float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

constexpr float kMistReach = 800.0f;          // the low ground measured within this (horizontally) of the eye
constexpr float kMistDepth = 25.0f;           // the layer's top over the low ground (world units)

}  // namespace

// A terrain draw's heights (its base pass): a few hundred of its vertices near the eye.
void Device::NoteTerrainHeights(uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    if (!vertices || vertexCount == 0 || m_mist <= 0.0f || m_mistSamples.size() >= 8192)
        return;
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    const uint32_t step = std::max(1u, vertexCount / 512u);
    for (uint32_t i = 0; i < vertexCount; i += step) {
        float p[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        const float dx = p[0] - m_frameEye[0], dz = p[2] - m_frameEye[2];
        if (dx * dx + dz * dz < kMistReach * kMistReach)
            m_mistSamples.push_back(p[1]);
    }
}

// Once a frame: the low ground, how much mist.
void Device::UpdateMist()
{
    const double now = m_frameClock;
    const bool first = m_mistClock < 0.0;        // (logging in: the mist as it is, not fading in)
    const float dt = first ? 0.0f : float(std::clamp(now - m_mistClock, 0.0, 0.25));
    m_mistClock = now;
    if (m_mistSamples.size() >= 32) {
        auto low = m_mistSamples.begin() + m_mistSamples.size() / 6;
        std::nth_element(m_mistSamples.begin(), low, m_mistSamples.end());
        const float target = *low;
        m_mistFloor = m_mistFloorSet ? m_mistFloor + (target - m_mistFloor) * (1.0f - std::exp(-dt / 10.0f)) : target;
        m_mistFloorSet = true;
    }
    m_mistSamples.clear();
    // The sun's height and whether it is rising (its height a minute ago).
    const float l = std::max(std::sqrt(m_trueSunDir[0] * m_trueSunDir[0] + m_trueSunDir[1] * m_trueSunDir[1] +
                                       m_trueSunDir[2] * m_trueSunDir[2]), 1e-6f);
    const float sunUp = -m_trueSunDir[1] / l;
    m_mistSunTrend += ((sunUp - m_mistSunPrev) - m_mistSunTrend) * (1.0f - std::exp(-dt / 60.0f));
    m_mistSunPrev = sunUp;
    const float lowSun = 1.0f - Smooth(-0.05f, 0.35f, sunUp);
    const float rising = m_mistSunTrend >= 0.0f ? 1.0f : 0.6f;
    // The sky: clear calm nights make the most; rain lately more (wet ground) - fading over an hour.
    const float sky = SkyCloudiness() * std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
    m_mistWet = std::max(m_mistWet * std::exp(-dt / 3600.0f), std::clamp(m_rain, 0.0f, 1.0f));
    const float target = lowSun * rising * (1.2f - 0.5f * sky) * (1.0f + 0.6f * m_mistWet) * m_mist;
    m_mistAmount = first ? target : m_mistAmount + (target - m_mistAmount) * (1.0f - std::exp(-dt / 20.0f));
    if ((m_frameNumber % 600) == 40)
        Log("mist: %.2f (sun %.2f, %s; sky %.2f, wet %.2f), the low ground %.0f, its top %.0f", double(m_mistAmount),
            double(sunUp), m_mistSunTrend >= 0.0f ? "rising" : "setting", double(sky), double(m_mistWet),
            double(m_mistFloor), double(m_mistFloor + kMistDepth));
}

// For the tone mapping (LandHaze): the layer's top over the eye (world units) and its density (per unit; 0: none).
void Device::MistParams(float out[2]) const
{
    out[0] = out[1] = 0.0f;
    if (!m_mistFloorSet || m_mistAmount < 0.01f || !m_aoProjValid)
        return;
    const auto& v = m_aoView.m;
    const float eyeY = -(v[3][0] * v[1][0] + v[3][1] * v[1][1] + v[3][2] * v[1][2]);
    out[0] = m_mistFloor + kMistDepth - eyeY;
    out[1] = 0.006f * m_mistAmount;              // (~300 units through it: most of the way to white)
}

}  // namespace rvk
