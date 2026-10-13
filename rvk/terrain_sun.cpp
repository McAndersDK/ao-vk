// The terrain's shade from a low sun (terrain_sun.frag). Low in the sky things' shadows are faded out (shadow maps
// can't give them - draw.cpp), and nothing knew a valley lay in the mountains' shade: our sunlight (leaves, grass,
// highlights, the game's light on things lit per pixel, the air's glow) lit it gold with the sun behind the mountains
// (user, 2026-10-13). The game's terrain as drawn (its base pass, world positions) is kept in a height grid round the
// eye (16 world units a cell, 256 a side; moved with the eye); from it, each cell's height to see the sun over the
// terrain (FL.terrainSun: ffp_main TerrainSun - per point, in the world) and the eye's own (FL.terrainSun2.x: grass,
// water, the air - drawn near the eye). Weighted in under ~20 degrees (full under ~10).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {
constexpr int kTsCells = 256;
constexpr float kTsCell = 16.0f;
constexpr float kTsReach = 3200.0f;              // how far towards the sun a hill is looked for
}

// The terrain's base pass: its vertices into the grid (each cell its highest).
void Device::NoteTerrainSun(uint32_t fvf, const void* vertices, uint32_t vertexCount)
{
    if (!vertices || vertexCount == 0)
        return;
    if (m_tsHeights.empty()) {
        m_tsHeights.assign(size_t(kTsCells) * kTsCells, -1e9f);
        m_tsOrigin[0] = int(std::floor(m_frameEye[0] / kTsCell)) - kTsCells / 2;
        m_tsOrigin[1] = int(std::floor(m_frameEye[2] / kTsCell)) - kTsCells / 2;
    }
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    for (uint32_t i = 0; i < vertexCount; ++i) {
        float p[3];
        std::memcpy(p, base + size_t(i) * layout.stride + layout.offset[0], sizeof(p));
        const int cx = int(std::floor(p[0] / kTsCell)) - m_tsOrigin[0], cz = int(std::floor(p[2] / kTsCell)) - m_tsOrigin[1];
        if (cx < 0 || cz < 0 || cx >= kTsCells || cz >= kTsCells)
            continue;
        float& h = m_tsHeights[size_t(cz) * kTsCells + cx];
        if (p[1] > h + 0.05f) {
            h = p[1];
            m_tsDirty = true;
        }
    }
}

// The grid moved with the eye (kept centred within an eighth of it): what it held shifted, the new edge unknown.
void Device::RecentreTerrainSun()
{
    const int ex = int(std::floor(m_frameEye[0] / kTsCell)) - kTsCells / 2;
    const int ez = int(std::floor(m_frameEye[2] / kTsCell)) - kTsCells / 2;
    const int dx = ex - m_tsOrigin[0], dz = ez - m_tsOrigin[1];
    if (std::abs(dx) < kTsCells / 8 && std::abs(dz) < kTsCells / 8)
        return;
    std::vector<float> moved(size_t(kTsCells) * kTsCells, -1e9f);
    if (std::abs(dx) < kTsCells && std::abs(dz) < kTsCells)
        for (int z = 0; z < kTsCells; ++z)
            for (int x = 0; x < kTsCells; ++x) {
                const int ox = x + dx, oz = z + dz;
                if (ox >= 0 && oz >= 0 && ox < kTsCells && oz < kTsCells)
                    moved[size_t(z) * kTsCells + x] = m_tsHeights[size_t(oz) * kTsCells + ox];
            }
    m_tsHeights.swap(moved);
    m_tsOrigin[0] = ex;
    m_tsOrigin[1] = ez;
    m_tsDirty = true;
}

// Once a frame, outside any rendering (RenderShadowMap): the grid moved with the eye; the shade map made again when
// the grid or the sun changed (or every second); the eye's own.
void Device::UpdateTerrainSun(VkCommandBuffer cmd)
{
    m_tsWeight = 0.0f;
    if (m_frameNumber % 600 == 40)
        Log("terrain sun: grid %s, pipeline %s, made %s", m_tsHeights.empty() ? "empty" : "kept",
            m_terrainSunPipeline ? "yes" : "no", m_tsMadeFrame == ~0ull ? "never" : "yes");
    if (m_tsHeights.empty() || !m_terrainSunPipeline || !m_hillShade)
        return;
    RecentreTerrainSun();
    float to[3] = {-m_trueSunDir[0], -m_trueSunDir[1], -m_trueSunDir[2]};
    const float l = std::sqrt(to[0] * to[0] + to[1] * to[1] + to[2] * to[2]);
    if (l < 1e-6f)
        return;
    for (float& v : to) v /= l;
    // (Weighted in under ~20 degrees, full under ~10: higher, things' shadows have it - and mountains rarely.)
    // (None once it's down: no sunlight to take - by night the ground's lightmap would only darken.)
    const float t = std::clamp((to[1] - 0.17f) / (0.35f - 0.17f), 0.0f, 1.0f);
    m_tsWeight = (1.0f - t * t * (3.0f - 2.0f * t)) * std::clamp((to[1] + 0.03f) / 0.03f, 0.0f, 1.0f);
    if (m_tsWeight <= 0.0f)
        return;
    const float flat = std::sqrt(to[0] * to[0] + to[2] * to[2]);
    const float ux = flat > 1e-4f ? to[0] / flat : 1.0f, uz = flat > 1e-4f ? to[2] / flat : 0.0f;
    const float climb = kTsCell * to[1] / std::max(flat, 1e-3f);   // (the sun's rise over a cell's way)
    // The eye's own: towards the sun over the grid.
    {
        float need = -1e9f;
        const float gx = m_frameEye[0] / kTsCell - float(m_tsOrigin[0]), gz = m_frameEye[2] / kTsCell - float(m_tsOrigin[1]);
        for (int k = 1; k <= int(kTsReach / kTsCell); ++k) {
            const int cx = int(std::floor(gx + ux * float(k))), cz = int(std::floor(gz + uz * float(k)));
            if (cx < 0 || cz < 0 || cx >= kTsCells || cz >= kTsCells)
                break;
            const float y = m_tsHeights[size_t(cz) * kTsCells + cx];
            if (y > -1e8f) need = std::max(need, y - climb * float(k));
        }
        const float s = std::clamp((m_frameEye[1] - (need - 1.0f)) / 4.0f, 0.0f, 1.0f);
        m_tsEye = s * s * (3.0f - 2.0f * s);
    }
    const bool sunMoved = to[0] * m_tsSunUsed[0] + to[1] * m_tsSunUsed[1] + to[2] * m_tsSunUsed[2] < 0.99999f;
    if (!m_tsDirty && !sunMoved && m_tsMadeFrame + 60 > m_frameNumber && m_terrainSunMap)
        return;
    if (!m_terrainSunMap || m_terrainSunMap->m_width != uint32_t(kTsCells)) {
        if (m_terrainSunMap) DestroyTexture(m_terrainSunMap);
        m_terrainSunMap = CreateImage(kTsCells, kTsCells, Format::RG16F, 1, true);
        if (!m_terrainSunMap)
            return;
    }
    const VkDeviceSize bytes = sizeof(float) * m_tsHeights.size();
    EnsureRingSpace(bytes + 4096);
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(bytes, std::max<VkDeviceSize>(256, m_props.limits.minStorageBufferOffsetAlignment), &cpu);
    std::memcpy(cpu, m_tsHeights.data(), size_t(bytes));
    Frame& f = m_frames[m_frameIndex];
    Transition(cmd, m_terrainSunMap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_terrainSunMap->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {uint32_t(kTsCells), uint32_t(kTsCells)}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const VkViewport viewport{0.0f, 0.0f, float(kTsCells), float(kTsCells), 0.0f, 1.0f};
    const VkRect2D scissor{{0, 0}, {uint32_t(kTsCells), uint32_t(kTsCells)}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrainSunPipeline);
    VkDescriptorBufferInfo heights{f.ring, offset, bytes};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    w.pBufferInfo = &heights;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_terrainSunLayout, 0, 1, &w);
    const float push[8] = {ux, uz, climb, kTsReach / kTsCell, float(kTsCells), 0.0f, 0.0f, 0.0f};
    vkCmdPushConstants(cmd, m_terrainSunLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_terrainSunMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_tsMapOrigin[0] = float(m_tsOrigin[0]) * kTsCell;
    m_tsMapOrigin[1] = float(m_tsOrigin[1]) * kTsCell;
    std::memcpy(m_tsSunUsed, to, sizeof(m_tsSunUsed));
    m_tsDirty = false;
    m_tsMadeFrame = m_frameNumber;
    if (m_frameNumber % 600 < 120) {   // (made seldom: logged when made near the log's time)
        float lo = 1e9f, hi = -1e9f;
        size_t known = 0;
        for (float h : m_tsHeights)
            if (h > -1e8f) { lo = std::min(lo, h); hi = std::max(hi, h); ++known; }
        Log("terrain sun: %zu of %d cells known, heights %.0f..%.0f; the eye (y %.1f) %.2f in the sun (weight %.2f)",
            known, kTsCells * kTsCells, double(lo), double(hi), double(m_frameEye[1]), double(m_tsEye), double(m_tsWeight));
    }
}

// The frame lights' part (draw.cpp): where the map lies, its bindless slot; the eye's own, the weight.
void Device::TerrainSunLights(float out[8])
{
    std::memset(out, 0, sizeof(float) * 8);
    out[4] = 1.0f;
    if (m_tsWeight <= 0.0f || !m_terrainSunMap || m_tsMadeFrame == ~0ull ||
        m_terrainSunMap->m_layout != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
        return;
    const uint32_t image = BindlessImage(m_terrainSunMap), sampler = FixedSamplerSlot(2, m_linearSampler);
    if (image == ~0u || image >= (1u << 20) - 1 || sampler >= 256)
        return;
    const uint32_t slot = (image + 1) | sampler << 20;
    out[0] = m_tsMapOrigin[0];
    out[1] = m_tsMapOrigin[1];
    out[2] = 1.0f / (kTsCell * float(kTsCells));
    std::memcpy(&out[3], &slot, 4);
    out[4] = m_tsEye;
    out[5] = m_tsWeight;
    out[6] = m_shadowStrength;                   // (its strength: the shadows' own, not faded low as theirs are)
}

}  // namespace rvk
