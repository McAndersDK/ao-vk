// Enhancement: the water (RVK_Water). The game's water - VisualLiquid_t: a world-space triangle mesh per liquid type
// (DisplaySystem's global arrays, FVF 0x152) that it transforms to the screen with ProcessVertices and draws flat,
// with a scrolled texture, in two or three blended passes - is replaced by our own surface: the proxy keeps the world
// vertices from the ProcessVertices call (rvk_device.cpp) and, at the game's first draw of them in the frame, hands
// them here instead (DrawWater); the game's draws of them are skipped.
//
// The game's water triangles are huge (it subdivides only edges over 10000 units), so they can't carry waves. We draw
// a polar grid around the camera instead (water.vert: rings growing geometrically, as dense as their distance needs;
// one mesh, no cracks), lifted onto the water by a water map - the game's flat triangles rasterised top-down into an
// image (R: the surface's height, G: water here) - and moved by Gerstner waves. Each connected body is baked flat at
// its own level (the game hangs shore aprons and skirts below the surface, which the coarse far grid would turn into
// terraces - "mountains of water"). Steep triangles (falls) are drawn as they are (mesh mode).
//
// Drawn where the game draws its water, in the scene's rendering: what is under and behind the water must be known,
// so the scene's colour and depth are copied first (once a frame) and the water drawn opaque over them (water.frag:
// refraction, absorption, caustics, reflections, foam, fog), writing depth as a surface does.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kWaterVertSpirv[] = {
#include "water.vert.inc"
};
const uint32_t kWaterFragSpirv[] = {
#include "water.frag.inc"
};
const uint32_t kSprayVertSpirv[] = {
#include "water_spray.vert.inc"
};
const uint32_t kSprayFragSpirv[] = {
#include "water_spray.frag.inc"
};
const uint32_t kWaterSimSpirv[] = {
#include "water_sim.comp.inc"
};

// The disturbance field (water_sim.comp): kSimN x kSimN cells (their size set by the quality), and the most characters
// it is handed (the nearest to the camera).
constexpr uint32_t kSimN = 256;
constexpr uint32_t kSimBodies = 32;

// The per-draw block (water_common.glsl WaterFrame, std140).
struct WaterFrame {
    d3d::Matrix viewProj, prevViewProj, view;
    float proj[4], camera[4], viewport[4], time[4], wind[4], map[4], mapInfo[4], grid[4];
    float sunColour[4], sunDir[4], ambient[4], tint[4], look[4], look2[4], fogColour[4], fogParams[4], sky[4];
    float env[4];
    float ripple[4];                             // x: the disturbance field is bound, y: strength, z: 1 / size, w: cell
    float fall[4];                               // x: falls' sheen (RVK_WaterSheen), y: their sparkles (RVK_WaterSpark),
                                                 // z: how many foot segments (binding 2 in the grid and spray draws)
    float fall2[4];                              // x: the spray (RVK_WaterSpray), y: the churn (RVK_WaterChurn),
                                                 // z: how fast the falls fall (RVK_WaterSpeed), w: the rain
};

// A fall's foot as the grid's foam and the spray read it (binding 2; water_common.glsl FootSegment): a segment of where
// it meets the pool, and the way out from the cliff there.
constexpr size_t kFootMax = 32;
constexpr uint32_t kSprayPerFoot = 48;          // particles a segment (water_spray.vert: its split into mist and drops)

constexpr uint32_t kDetailSize = 256;
constexpr uint32_t kMapMax = 1024;              // water map side (texels)
constexpr float kGridFirstRing = 0.25f;         // world units
constexpr uint32_t kGridMaxRings = 720;

void ArgbTo4(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

float AsFloatBits(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

uint16_t ToHalf(float f)
{
    uint32_t x;
    std::memcpy(&x, &f, 4);
    uint32_t sign = (x >> 16) & 0x8000u;
    int32_t exp = int32_t((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return uint16_t(sign);                  // tiny: zero (the map's heights are never that small and non-zero)
    if (exp >= 31)
        return uint16_t(sign | 0x7BFFu);        // the largest finite half
    uint32_t h = sign | (uint32_t(exp) << 10) | (mant >> 13);
    if (mant & 0x1000u)
        ++h;                                    // round half up
    return uint16_t(h);
}

// A small deterministic random sequence (the detail texture is the same every run).
struct Rng {
    uint32_t s;
    float Next() { s = s * 1664525u + 1013904223u; return float(s >> 8) / 16777216.0f; }
};

// Each level of the detail texture (RGBA8): RG the ripples' slope (a sum of waves with whole wave numbers, so it
// tiles), B a soft noise (the foam's breakup), A the cells' edges (F2 - F1 of a tiled cell noise: the caustics'
// web). Then box-filtered mip levels.
std::vector<std::vector<uint8_t>> MakeDetail()
{
    const uint32_t n = kDetailSize;
    std::vector<float> sx(n * n), sz(n * n), noise(n * n), cells(n * n);
    Rng rng{12345u};
    const float twoPi = 6.2831853f;
    struct Wave { float kx, kz, amp, phase; };
    std::vector<Wave> ripples, soft;
    for (int i = 0; i < 48; ++i) {
        int kx = 0, kz = 0;
        while (kx == 0 && kz == 0) {
            kx = int(rng.Next() * 25.0f) - 12;
            kz = int(rng.Next() * 25.0f) - 12;
        }
        float k = std::sqrt(float(kx * kx + kz * kz));
        ripples.push_back({float(kx), float(kz), 1.0f / std::pow(k, 1.6f), rng.Next() * twoPi});
    }
    for (int i = 0; i < 24; ++i) {
        int kx = int(rng.Next() * 13.0f) - 6, kz = int(rng.Next() * 13.0f) - 6;
        if (kx == 0 && kz == 0) kx = 1;
        float k = std::sqrt(float(kx * kx + kz * kz));
        soft.push_back({float(kx), float(kz), 1.0f / k, rng.Next() * twoPi});
    }
    // Cell points: 6 x 6 cells, one point each (tiled by wrapping).
    const int cellsPerSide = 6;
    std::vector<float> px(cellsPerSide * cellsPerSide), pz(px.size());
    for (size_t i = 0; i < px.size(); ++i) {
        px[i] = rng.Next();
        pz[i] = rng.Next();
    }
    float maxSlope = 0.0f, minNoise = 1e9f, maxNoise = -1e9f, maxCell = 0.0f;
    for (uint32_t y = 0; y < n; ++y)
        for (uint32_t x = 0; x < n; ++x) {
            float u = float(x) / float(n), v = float(y) / float(n);
            float gx = 0.0f, gz = 0.0f, h = 0.0f;
            for (const Wave& w : ripples) {
                float th = twoPi * (w.kx * u + w.kz * v) + w.phase;
                float c = std::cos(th);
                gx += w.amp * w.kx * c;
                gz += w.amp * w.kz * c;
            }
            for (const Wave& w : soft)
                h += w.amp * std::sin(twoPi * (w.kx * u + w.kz * v) + w.phase);
            // The cells' domain warped (whole frequencies: it still tiles), so their edges curve like caustics do.
            float wu = u + 0.045f * std::sin(twoPi * (2.0f * v + 0.3f)) + 0.03f * std::sin(twoPi * (3.0f * u + 5.0f * v));
            float wv = v + 0.045f * std::sin(twoPi * (2.0f * u + 0.7f)) + 0.03f * std::sin(twoPi * (4.0f * u - 3.0f * v));
            float cu = wu * cellsPerSide, cv = wv * cellsPerSide;
            int ci = int(std::floor(cu)), cj = int(std::floor(cv));
            float f1 = 1e9f, f2 = 1e9f;
            for (int dj = -1; dj <= 1; ++dj)
                for (int di = -1; di <= 1; ++di) {
                    int i = ci + di, j = cj + dj;
                    int wi = ((i % cellsPerSide) + cellsPerSide) % cellsPerSide;
                    int wj = ((j % cellsPerSide) + cellsPerSide) % cellsPerSide;
                    float ex = float(i) + px[size_t(wj * cellsPerSide + wi)] - cu;
                    float ez = float(j) + pz[size_t(wj * cellsPerSide + wi)] - cv;
                    float d = std::sqrt(ex * ex + ez * ez);
                    if (d < f1) { f2 = f1; f1 = d; } else if (d < f2) f2 = d;
                }
            size_t idx = size_t(y) * n + x;
            sx[idx] = gx;
            sz[idx] = gz;
            noise[idx] = h;
            cells[idx] = f2 - f1;
            maxSlope = std::max({maxSlope, std::fabs(gx), std::fabs(gz)});
            minNoise = std::min(minNoise, h);
            maxNoise = std::max(maxNoise, h);
            maxCell = std::max(maxCell, f2 - f1);
        }
    std::vector<std::vector<uint8_t>> levels;
    std::vector<float> rgba(size_t(n) * n * 4);
    auto q = [](float v) { return uint8_t(std::clamp(v, 0.0f, 1.0f) * 255.0f + 0.5f); };
    for (size_t i = 0; i < size_t(n) * n; ++i) {
        rgba[i * 4 + 0] = 0.5f + 0.5f * sx[i] / maxSlope;
        rgba[i * 4 + 1] = 0.5f + 0.5f * sz[i] / maxSlope;
        rgba[i * 4 + 2] = (noise[i] - minNoise) / std::max(maxNoise - minNoise, 1e-6f);
        rgba[i * 4 + 3] = std::min(cells[i] / (maxCell * 0.6f), 1.0f);
    }
    uint32_t size = n;
    for (;;) {
        std::vector<uint8_t> level(size_t(size) * size * 4);
        for (size_t i = 0; i < level.size(); ++i)
            level[i] = q(rgba[i]);
        levels.push_back(std::move(level));
        if (size == 1)
            break;
        uint32_t half = size / 2;
        std::vector<float> next(size_t(half) * half * 4);
        for (uint32_t y = 0; y < half; ++y)
            for (uint32_t x = 0; x < half; ++x)
                for (int c = 0; c < 4; ++c) {
                    auto at = [&](uint32_t xx, uint32_t yy) { return rgba[(size_t(yy) * size + xx) * 4 + c]; };
                    next[(size_t(y) * half + x) * 4 + c] =
                        0.25f * (at(2 * x, 2 * y) + at(2 * x + 1, 2 * y) + at(2 * x, 2 * y + 1) + at(2 * x + 1, 2 * y + 1));
                }
        rgba = std::move(next);
        size = half;
    }
    return levels;
}

uint64_t HashWater(const Device::WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount)
{
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](const void* data, size_t bytes) {
        const uint8_t* p = static_cast<const uint8_t*>(data);
        for (size_t i = 0; i < bytes; ++i)
            h = (h ^ p[i]) * 1099511628211ull;
    };
    for (uint32_t i = 0; i < count; ++i)
        mix(v[i].pos, 12);
    mix(idx, size_t(icount) * 2);
    return h ^ count;
}

}  // namespace

bool Device::CreateWaterResources(std::string* error)
{
    DestroyWaterResources();
    // One pushed set: 0 the block, 2 the mesh's vertices, 4 the frame lights, 5 the sun's shadows, 7 the water map, 8
    // the scene's depth, 9 the scene, 10 the detail texture, 11 the game's texture, 12 the environment probe's atlas, 13 the disturbance field,
    // 14 the clouds' shadow, 15 the terrain's shade (terrain_sun.cpp).
    const uint32_t numbers[13] = {0, 2, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    VkDescriptorSetLayoutBinding b[13] = {};
    const VkShaderStageFlags both = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    for (int i = 0; i < 13; ++i) {
        b[i].binding = numbers[i];
        b[i].descriptorCount = 1;
        b[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        b[i].stageFlags = both;
    }
    b[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    b[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 13;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_waterSetLayout), "vkCreateDescriptorSetLayout",
               error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_waterSetLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_waterLayout), "vkCreatePipelineLayout", error))
        return false;
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = module(kWaterVertSpirv, sizeof(kWaterVertSpirv), &vert) &&
              module(kWaterFragSpirv, sizeof(kWaterFragSpirv), &frag);
    if (ok) {
        VkPipelineShaderStageCreateInfo stages[2] = {
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr},
            {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr},
        };
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        ds.dynamicStateCount = 2;
        ds.pDynamicStates = dyn;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;         // seen from below too (the camera under the water)
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = VK_TRUE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        VkPipelineColorBlendAttachmentState att[5] = {};
        for (auto& a : att)
            a.colorWriteMask = 0xF;              // colour, glow, local fraction, motion, albedo: a solid surface
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 5;
        cb.pAttachments = att;
        VkFormat colorFormats[5] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA16F).vk,
                                    GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RGBA16F).vk,
                                    GetFormatInfo(Format::A8R8G8B8).vk};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 5;
        rendering.pColorAttachmentFormats = colorFormats;
        rendering.depthAttachmentFormat = kDepthFormat;
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        ci.pNext = &rendering;
        ci.stageCount = 2;
        ci.pStages = stages;
        ci.pVertexInputState = &vi;
        ci.pInputAssemblyState = &ia;
        ci.pViewportState = &vp;
        ci.pRasterizationState = &rs;
        ci.pMultisampleState = &ms;
        ci.pDepthStencilState = &dss;
        ci.pColorBlendState = &cb;
        ci.pDynamicState = &ds;
        ci.layout = m_waterLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_waterPipeline),
                   "vkCreateGraphicsPipelines", error);
        // The falls' spray: the same set, its own shaders; blended over the scene's colour (premultiplied), depth
        // tested but not written. (All targets blend alike - no independent blending - the shader gives the others
        // nothing: premultiplied 0 leaves them as they are.) Without it the falls only lose their spray.
        VkShaderModule sprayVert = VK_NULL_HANDLE, sprayFrag = VK_NULL_HANDLE;
        std::string sprayError;
        if (ok && module(kSprayVertSpirv, sizeof(kSprayVertSpirv), &sprayVert) &&
            module(kSprayFragSpirv, sizeof(kSprayFragSpirv), &sprayFrag)) {
            stages[0].module = sprayVert;
            stages[1].module = sprayFrag;
            dss.depthWriteEnable = VK_FALSE;
            for (auto& a : att) {
                a.colorWriteMask = 0xF;
                a.blendEnable = VK_TRUE;
                a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            if (!Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_waterSprayPipeline),
                       "vkCreateGraphicsPipelines (spray)", &sprayError))
                Log("water spray unavailable: %s\n", sprayError.c_str());
        }
        if (sprayVert) vkDestroyShaderModule(m_device, sprayVert, nullptr);
        if (sprayFrag) vkDestroyShaderModule(m_device, sprayFrag, nullptr);
    }
    if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
    if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
    if (!ok)
        return false;
    // Samplers: the detail texture tiles (mipmapped, anisotropic); the game's texture tiles too.
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = VK_LOD_CLAMP_NONE;
    if (m_maxAnisotropy >= 1.0f) {
        si.anisotropyEnable = VK_TRUE;
        si.maxAnisotropy = std::min(m_maxAnisotropy, 8.0f);
    }
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_waterRepeatSampler), "water sampler", error))
        return false;
    // The detail texture.
    std::vector<std::vector<uint8_t>> levels = MakeDetail();
    m_waterDetail = CreateImage(kDetailSize, kDetailSize, Format::RGBA8, uint32_t(levels.size()), false);
    if (!m_waterDetail) {
        if (error) *error = "water detail texture";
        return false;
    }
    for (uint32_t l = 0; l < levels.size(); ++l) {
        uint32_t s = std::max(kDetailSize >> l, 1u);
        UpdateTexture(m_waterDetail, l, 0, 0, s, s, levels[l].data(), s * 4);
    }
    std::string simError;
    if (!CreateWaterSim(&simError))              // (the water works without it, only the characters' ripples are lost)
        Log("water disturbance field unavailable: %s\n", simError.c_str());
    return true;
}

void Device::DestroyWaterResources()
{
    m_waterReady.store(false, std::memory_order_relaxed);
    for (WaterMap& m : m_waterMaps)
        if (m.texture) DestroyTextureNow(m.texture);
    m_waterMaps.clear();
    m_waterFalls.clear();
    m_waterFoot.clear();
    if (m_waterDetail) { DestroyTextureNow(m_waterDetail); m_waterDetail = nullptr; }
    if (m_waterSceneCopy) { DestroyTextureNow(m_waterSceneCopy); m_waterSceneCopy = nullptr; }
    if (m_waterDepthCopy) {
        vkDestroyImageView(m_device, m_waterDepthCopyView, nullptr);
        vmaDestroyImage(m_allocator, m_waterDepthCopy, m_waterDepthCopyAllocation);
        m_waterDepthCopy = VK_NULL_HANDLE;
        m_waterDepthCopyView = VK_NULL_HANDLE;
        m_waterDepthCopyAllocation = nullptr;
    }
    for (WaterGrid& g : m_waterGrids)
        if (g.buffer) vmaDestroyBuffer(m_allocator, g.buffer, g.allocation);
    m_waterGrids.clear();
    for (int i = 0; i < 2; ++i) {
        if (m_waterSimView[i]) { vkDestroyImageView(m_device, m_waterSimView[i], nullptr); m_waterSimView[i] = VK_NULL_HANDLE; }
        if (m_waterSim[i]) { vmaDestroyImage(m_allocator, m_waterSim[i], m_waterSimAllocation[i]); m_waterSim[i] = VK_NULL_HANDLE; }
    }
    if (m_waterSimPipeline) { vkDestroyPipeline(m_device, m_waterSimPipeline, nullptr); m_waterSimPipeline = VK_NULL_HANDLE; }
    if (m_waterSimLayout) { vkDestroyPipelineLayout(m_device, m_waterSimLayout, nullptr); m_waterSimLayout = VK_NULL_HANDLE; }
    if (m_waterSimSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_waterSimSetLayout, nullptr); m_waterSimSetLayout = VK_NULL_HANDLE; }
    m_waterSimLive = false;
    m_waterSimClear = true;
    if (m_waterRepeatSampler) { vkDestroySampler(m_device, m_waterRepeatSampler, nullptr); m_waterRepeatSampler = VK_NULL_HANDLE; }
    if (m_waterPipeline) { vkDestroyPipeline(m_device, m_waterPipeline, nullptr); m_waterPipeline = VK_NULL_HANDLE; }
    if (m_waterSprayPipeline) { vkDestroyPipeline(m_device, m_waterSprayPipeline, nullptr); m_waterSprayPipeline = VK_NULL_HANDLE; }
    if (m_waterLayout) { vkDestroyPipelineLayout(m_device, m_waterLayout, nullptr); m_waterLayout = VK_NULL_HANDLE; }
    if (m_waterSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_waterSetLayout, nullptr); m_waterSetLayout = VK_NULL_HANDLE; }
    m_waterDepthW = m_waterDepthH = 0;
}

// Whether the water can be drawn now (the proxy hands the game's water over only then): at the frame's start.
void Device::UpdateWaterReady()
{
    m_waterReady.store(m_waterParams.enable && m_waterPipeline && m_waterDetail && m_hdr,
                       std::memory_order_relaxed);
}

// The grid's index buffer for a number of segments around (quads between ring i and i + 1, ring-major: the first k
// rings' triangles are a prefix).
const Device::WaterGrid* Device::WaterGridFor(uint32_t segments)
{
    for (const WaterGrid& g : m_waterGrids)
        if (g.segments == segments)
            return &g;
    const VkDeviceSize count = VkDeviceSize(segments) * kGridMaxRings * 6;
    const VkDeviceSize bytes = count * sizeof(uint32_t);
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = bytes;
    bi.usage = VK_BUFFER_USAGE_INDEX_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    WaterGrid g;
    g.segments = segments;
    std::string err;
    if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &g.buffer, &g.allocation, nullptr), "water grid", &err))
        return nullptr;
    EnsureRingSpace(bytes + 64);
    void* cpu;
    const VkDeviceSize staging = Allocate(bytes, 16, &cpu);
    uint32_t* out = static_cast<uint32_t*>(cpu);
    for (uint32_t r = 0; r < kGridMaxRings; ++r)
        for (uint32_t s = 0; s < segments; ++s) {
            uint32_t s1 = (s + 1) % segments;
            uint32_t a = r * segments + s, b = r * segments + s1;
            uint32_t c = (r + 1) * segments + s, d = (r + 1) * segments + s1;
            *out++ = a; *out++ = c; *out++ = d;
            *out++ = a; *out++ = d; *out++ = b;
        }
    VkBufferCopy region{staging, 0, bytes};
    vkCmdCopyBuffer(UploadCommands(), m_frames[m_frameIndex].ring, g.buffer, 1, &region);
    m_skinUploadsPending = true;
    m_waterGrids.push_back(g);
    return &m_waterGrids.back();
}

// The water map of a mesh (made the first time it is seen; the last few kept): its flat triangles rasterised
// top-down. steep: the triangles left out (falls), as indices into the mesh.
Device::WaterMap* Device::WaterMapFor(const WaterVertex* v, uint32_t count, const uint16_t* idx, uint32_t icount,
                                      std::vector<uint32_t>& steep)
{
    steep.clear();
    struct Tri { uint32_t a, b, c; };
    std::vector<Tri> flat;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    float flatLoY = 1e30f, flatHiY = -1e30f;         // the flat triangles' height span (a step = mountains)
    double heightSum = 0.0;
    for (uint32_t t = 0; t + 2 < icount; t += 3) {
        uint32_t i0 = idx[t], i1 = idx[t + 1], i2 = idx[t + 2];
        if (i0 >= count || i1 >= count || i2 >= count)
            continue;
        const float* p0 = v[i0].pos;
        const float* p1 = v[i1].pos;
        const float* p2 = v[i2].pos;
        float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float n[3] = {e1[1] * e2[2] - e1[2] * e2[1], e1[2] * e2[0] - e1[0] * e2[2], e1[0] * e2[1] - e1[1] * e2[0]};
        float len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
        if (len <= 1e-8f)
            continue;
        if (std::fabs(n[1]) / len < 0.9f) {
            steep.insert(steep.end(), {i0, i1, i2});
            continue;
        }
        flat.push_back({i0, i1, i2});
        for (const float* p : {p0, p1, p2}) {
            flatLoY = std::min(flatLoY, p[1]);
            flatHiY = std::max(flatHiY, p[1]);
            for (int k = 0; k < 3; ++k) {
                lo[k] = std::min(lo[k], p[k]);
                hi[k] = std::max(hi[k], p[k]);
            }
        }
        heightSum += double(p0[1] + p1[1] + p2[1]) / 3.0;
    }
    if (flat.empty())
        return nullptr;
    const uint64_t key = HashWater(v, count, idx, icount);
    for (WaterMap& m : m_waterMaps)
        if (m.key == key) {
            m.lastFrame = m_frameNumber;
            return &m;
        }
    // A new map: in place of the one not used the longest once a few are kept.
    if (m_waterMaps.size() >= 4) {
        auto oldest = std::min_element(m_waterMaps.begin(), m_waterMaps.end(),
                                       [](const WaterMap& a, const WaterMap& b) { return a.lastFrame < b.lastFrame; });
        if (oldest->texture) DestroyTexture(oldest->texture);
        m_waterMaps.erase(oldest);
    }
    WaterMap m;
    m.key = key;
    m.lastFrame = m_frameNumber;
    m.base = float(heightSum / double(flat.size()));
    const float ex = hi[0] - lo[0], ez = hi[2] - lo[2];
    float texel = std::max(std::max(ex, ez) / float(kMapMax - 2), 0.5f);
    const uint32_t w = std::min(uint32_t(std::ceil(ex / texel)) + 2, kMapMax);
    const uint32_t h = std::min(uint32_t(std::ceil(ez / texel)) + 2, kMapMax);
    m.origin[0] = lo[0] - texel;
    m.origin[1] = lo[2] - texel;
    m.size[0] = float(w) * texel;
    m.size[1] = float(h) * texel;
    m.texel = texel;
    m.extent = std::max(ex, ez);
    m.lo[0] = lo[0]; m.lo[1] = lo[2];
    m.hi[0] = hi[0]; m.hi[1] = hi[2];
    m.flatTriangles = uint32_t(flat.size());
    m.colour = v[flat.front().a].colour;
    std::vector<uint16_t> pixels(size_t(w) * h * 4, 0);
    std::vector<float> heights(size_t(w) * h, 0.0f);  // wet texels' surface height, y - m.base (written below)
    const uint16_t one = ToHalf(1.0f);
    for (const Tri& t : flat) {
        const float* a = v[t.a].pos;
        const float* b = v[t.b].pos;
        const float* c = v[t.c].pos;
        float minX = std::min({a[0], b[0], c[0]}), maxX = std::max({a[0], b[0], c[0]});
        float minZ = std::min({a[2], b[2], c[2]}), maxZ = std::max({a[2], b[2], c[2]});
        // (Never the outermost texels: they stay dry, so what the clamped sampler reads beyond the map is no water - a
        // small pool's slack once reached them and flooded everything around it at its height.)
        int x0 = std::max(int(std::floor((minX - m.origin[0]) / texel)) - 1, 1);
        int x1 = std::min(int(std::ceil((maxX - m.origin[0]) / texel)) + 1, int(w) - 2);
        int z0 = std::max(int(std::floor((minZ - m.origin[1]) / texel)) - 1, 1);
        int z1 = std::min(int(std::ceil((maxZ - m.origin[1]) / texel)) + 1, int(h) - 2);
        float det = (b[2] - c[2]) * (a[0] - c[0]) + (c[0] - b[0]) * (a[2] - c[2]);
        if (std::fabs(det) < 1e-12f)
            continue;
        // A texel counts as water when its centre is inside, with half a texel to spare (neighbouring triangles leave
        // no seam between them).
        const float slack = 0.5f * texel / std::sqrt(std::fabs(det)) * 2.0f;
        for (int z = z0; z <= z1; ++z)
            for (int x = x0; x <= x1; ++x) {
                float px = m.origin[0] + (float(x) + 0.5f) * texel, pz = m.origin[1] + (float(z) + 0.5f) * texel;
                float l0 = ((b[2] - c[2]) * (px - c[0]) + (c[0] - b[0]) * (pz - c[2])) / det;
                float l1 = ((c[2] - a[2]) * (px - c[0]) + (a[0] - c[0]) * (pz - c[2])) / det;
                float l2 = 1.0f - l0 - l1;
                if (l0 < -slack || l1 < -slack || l2 < -slack)
                    continue;
                float y = l0 * a[1] + l1 * b[1] + l2 * c[1];
                size_t idx = size_t(z) * w + x;
                heights[idx] = y - m.base;
                pixels[idx * 4 + 0] = 0;             // the height is written once the levels are known (below)
                pixels[idx * 4 + 1] = one;           // water here
            }
    }
    // A water body's mesh hangs shore aprons and skirts below its surface, holds thin high tails (a waterfall's lip),
    // and one mesh may hold several bodies; far from the camera the polar grid is coarse enough to build those steps
    // into "mountains of water". Split the wet texels into connected bodies (a flood fill) and give each one flat its
    // own surface height, so each body reads as one sheet at its level.
    std::vector<int32_t> body(size_t(w) * h, -1);        // a wet texel's body (-1 = dry)
    std::vector<float> bodyLevel;                        // each body's level, relative to m.base (the mean height)
    std::vector<int32_t> stack;
    // (A body whose heights run over a wide span is a slope - a river, a cascade of pools - not one sheet: it keeps its
    // texels' own heights. One level for all of it put a pond up a mountain under the ground.)
    constexpr float kFlatSpan = 4.0f;
    std::vector<uint8_t> bodyKeep;                       // 1: not flattened
    for (uint32_t z = 0; z < h; ++z)
        for (uint32_t x = 0; x < w; ++x) {
            const size_t start = size_t(z) * w + x;
            if (!pixels[start * 4 + 1] || body[start] >= 0)
                continue;
            const int id = int(bodyLevel.size());
            stack.assign(1, int32_t(start));
            body[start] = id;
            std::vector<float> hs;
            while (!stack.empty()) {
                const int32_t cur = stack.back();
                stack.pop_back();
                hs.push_back(heights[size_t(cur)]);
                const uint32_t cz = uint32_t(cur) / w, cx = uint32_t(cur) % w;
                const int32_t n[4] = {cx ? cur - 1 : -1, cx + 1 < w ? cur + 1 : -1,
                                      cz ? cur - int32_t(w) : -1, cz + 1 < h ? cur + int32_t(w) : -1};
                for (int32_t k : n)
                    if (k >= 0 && pixels[size_t(k) * 4 + 1] && body[size_t(k)] < 0) {
                        body[size_t(k)] = id;
                        stack.push_back(k);
                    }
            }
            // The level: the body's flat surface - the highest height a substantial part of its area lies at (a
            // plateau). This ignores both a thin high tail (a waterfall's lip, which pulls a top percentile up) and a
            // spread-out low apron (which pulls a most-common height down). Each body is then flat at its own level.
            float mn = 1e30f, mx = -1e30f;
            for (float h : hs) { mn = std::min(mn, h); mx = std::max(mx, h); }
            const float binW = 0.5f;                 // world units: the level's resolution
            const int nb = std::min(8192, std::max(1, int((mx - mn) / binW) + 1));
            std::vector<uint64_t> area(size_t(nb), 0);
            for (float h : hs)
                ++area[size_t(std::min(nb - 1, std::max(0, int((h - mn) / binW))))];
            uint64_t maxArea = 0;
            for (uint64_t c : area)
                maxArea = std::max(maxArea, c);
            int levelBin = 0;
            for (int b = nb - 1; b >= 0; --b)
                if (area[size_t(b)] * 10 >= maxArea) {   // at least a tenth of the fullest: a plateau, not a tail
                    levelBin = b;
                    break;
                }
            int loBin = levelBin;                    // the plateau's whole run of bins (the surface, not just its top)
            while (loBin > 0 && area[size_t(loBin - 1)] * 10 >= maxArea)
                --loBin;
            float level = mn + (float(levelBin) + 0.5f) * binW;
            {
                double sum = 0.0;
                uint64_t count = 0;
                for (float h : hs) {
                    const int b = std::min(nb - 1, std::max(0, int((h - mn) / binW)));
                    if (b >= loBin && b <= levelBin) { sum += double(h); ++count; }
                }
                if (count)
                    level = float(sum / double(count));
            }
            bodyKeep.push_back(mx - mn > kFlatSpan ? 1 : 0);
            bodyLevel.push_back(mx - mn > kFlatSpan ? mn : level);
        }
    // The shader adds the map's base to the stored height: put the lowest level in the base and store each texel at
    // its body's level (constant across the body), so every body reads as one flat sheet at its own height (a slope:
    // at its texels' own heights).
    float minLevel = 0.0f;
    if (!bodyLevel.empty())
        minLevel = *std::min_element(bodyLevel.begin(), bodyLevel.end());
    for (size_t i = 0; i < heights.size(); ++i) {
        if (!pixels[i * 4 + 1])
            continue;
        pixels[i * 4 + 0] = ToHalf((bodyKeep[size_t(body[i])] ? heights[i] : bodyLevel[size_t(body[i])]) - minLevel);
    }
    m.base += minLevel;
    m.texture = CreateImage(w, h, Format::RGBA16F, 1, false);
    if (!m.texture)
        return nullptr;
    UpdateTexture(m.texture, 0, 0, 0, w, h, pixels.data(), w * 8);
    m_waterMaps.push_back(m);
    // (The vertex alphas, flat and steep: the game fades its water's shore edges out by them.)
    uint32_t flatLo = 255, flatHi = 0, steepLo = 255, steepHi = 0;
    for (const Tri& t : flat)
        for (uint32_t i : {t.a, t.b, t.c}) { uint32_t a = v[i].colour >> 24; flatLo = std::min(flatLo, a); flatHi = std::max(flatHi, a); }
    for (uint32_t i : steep) { uint32_t a = v[i].colour >> 24; steepLo = std::min(steepLo, a); steepHi = std::max(steepHi, a); }
    WaterLog("water map %016llx: %u flat + %u steep triangles, %.0f x %.0f units, %zu bodies at y %.2f..%.2f, "
             "%u x %u texels of %.2f; vertex alpha flat %u..%u, steep %u..%u; flat triangle heights %.2f..%.2f",
             (unsigned long long)key, m.flatTriangles, uint32_t(steep.size() / 3), double(ex), double(ez),
             bodyLevel.size(), double(m.base),
             bodyLevel.empty() ? double(m.base)
                               : double(m.base + (*std::max_element(bodyLevel.begin(), bodyLevel.end()) - minLevel)),
             w, h, double(texel), flatLo, flatHi, steep.empty() ? 0u : steepLo, steepHi,
             double(flatLoY), double(flatHiY));
    return &m_waterMaps.back();
}

void Device::WaterLog(const char* fmt, ...)
{
    if (m_waterLogCount >= 64)
        return;
    ++m_waterLogCount;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    Log("%s", buf);
}

// The characters of the last completed frame, merged into bodies (a character is a few parts, each its own box) with
// their velocity, for the water's disturbance field (StepWaterSim). Once a frame, before the scene draws.
void Device::CollectWaterBodies()
{
    if (!m_waterParams.enable && !m_smokeParams.enable && !SandActive()) {   // (the smoke and sand are stirred by them too)
        m_waterBodies.clear();
        return;
    }
    double now = SwayClock();
    float dt = float(now - m_waterBodiesTime);
    m_waterBodiesTime = now;
    if (!(dt > 0.0f) || dt > 0.25f)
        dt = 1.0f / 60.0f;
    m_waterDt = dt;
    const float vdt = std::max(dt, 1.0f / 240.0f);   // (a very short frame must not turn a box's jitter into speed)

    // Merge the boxes that are one character (its parts, a head, a helmet), as the grass push does.
    constexpr float kMerge = 0.9f;
    struct Acc { float sx = 0, sz = 0, minY = 1e30f, maxY = -1e30f; int parts = 0; };
    std::vector<Acc> acc;
    for (const TessCharacter& c : m_tessCharsPrev) {   // (x, z, minY, maxY): where characters were drawn last frame
        Acc* into = nullptr;
        for (Acc& a : acc) {
            float dx = a.sx / a.parts - c.x, dz = a.sz / a.parts - c.z;
            if (dx * dx + dz * dz < kMerge * kMerge && c.minY < a.maxY + 0.5f && c.maxY > a.minY - 0.5f) {
                into = &a;
                break;
            }
        }
        if (!into) {
            acc.push_back({c.x, c.z, c.minY, c.maxY, 1});
            continue;
        }
        into->sx += c.x; into->sz += c.z; ++into->parts;
        into->minY = std::min(into->minY, c.minY);
        into->maxY = std::max(into->maxY, c.maxY);
    }
    if (acc.size() > 256)
        acc.resize(256);                             // (only the nearest few reach the water; a crowd needs no more)
    std::vector<WaterBody> cur;
    cur.reserve(acc.size());
    for (const Acc& a : acc) {
        WaterBody b;
        b.x = a.sx / a.parts; b.z = a.sz / a.parts;
        b.minY = a.minY;
        b.centreY = 0.5f * (a.minY + a.maxY);
        b.prevMinY = b.minY;
        // The nearest body of the last frame is the same character: how it moved says how it stirs the water.
        float best = 9.0f;
        const WaterBody* m = nullptr;
        for (const WaterBody& p : m_waterBodies) {
            float dx = b.x - p.x, dz = b.z - p.z, d = dx * dx + dz * dz;
            if (d < best) { best = d; m = &p; }
        }
        if (m) {
            const float k = 1.0f - std::exp(-vdt * 20.0f);   // smoothed (over ~50 ms), so a jittery box doesn't flicker it
            b.vx = m->vx + ((b.x - m->x) / vdt - m->vx) * k;
            b.vz = m->vz + ((b.z - m->z) / vdt - m->vz) * k;
            b.phase = m->phase;
            const float kf = 1.0f - std::exp(-vdt * 40.0f);
            b.fall = m->fall + (-(b.centreY - m->centreY) / vdt - m->fall) * kf;
            b.prevMinY = m->minY;
        } else {
            m_waterPhaseSeed = m_waterPhaseSeed * 1664525u + 1013904223u;   // a new body: a new, steady offset
            b.phase = float((m_waterPhaseSeed >> 8) & 0xFFFFu) / 65536.0f * 6.2831853f;
        }
        b.speed = std::sqrt(b.vx * b.vx + b.vz * b.vz);
        cur.push_back(b);
    }
    m_waterBodies = std::move(cur);
}

// The disturbance field of the water: a damped wave equation on a grid round the camera, stirred by the characters in
// the water (water_sim.comp), stepped once a frame between the scene's render passes. The water then reads it for its
// normals and foam.
bool Device::CreateWaterSim(std::string* error)
{
    VkDescriptorSetLayoutBinding b[4] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 4;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_waterSimSetLayout), "water sim set layout", error))
        return false;
    VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 64};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_waterSimSetLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_waterSimLayout), "water sim layout", error))
        return false;
    VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    mi.codeSize = sizeof(kWaterSimSpirv);
    mi.pCode = kWaterSimSpirv;
    VkShaderModule module;
    if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "water sim module", error))
        return false;
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module, "main", nullptr};
    ci.layout = m_waterSimLayout;
    bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_waterSimPipeline),
                    "water sim pipeline", error);
    vkDestroyShaderModule(m_device, module, nullptr);
    if (!ok)
        return false;
    for (int i = 0; i < 2; ++i) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_2D;
        ic.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        ic.extent = {kSimN, kSimN, 1};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateImage(m_allocator, &ic, &ac, &m_waterSim[i], &m_waterSimAllocation[i], nullptr),
                   "water sim image", error))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_waterSim[i];
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_waterSimView[i]), "water sim view", error))
            return false;
    }
    m_waterSimClear = true;
    return true;
}

void Device::StepWaterSim(VkCommandBuffer cmd, const WaterMap* map)
{
    m_waterSimLive = false;
    const WaterParams& P = m_waterParams;
    if (!m_waterSimPipeline || !m_waterSim[0] || !map || !(P.swimRipples > 0.0f))
        return;
    const auto& vm = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    const double now = m_waterBodiesTime;
    const float cell = P.quality == 1 ? 0.35f : P.quality >= 3 ? 0.2f : 0.25f;
    // Start afresh when the field can't be continued: another cell size, the camera far from where it was (a jump), or
    // the water unseen for a while (the field would be stale).
    const float jump = std::hypot(eye[0] - m_waterSimCam[0], eye[2] - m_waterSimCam[1]);
    if (cell != m_waterSimCell || jump > 2.5f || now - m_waterSimLastTime > 1.0)
        m_waterSimClear = true;
    m_waterSimCell = cell;
    m_waterSimCam[0] = eye[0];
    m_waterSimCam[1] = eye[2];
    m_waterSimLastTime = now;
    m_waterSimTime += m_waterDt;

    auto memory = [&](VkPipelineStageFlags2 srcStage, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dstStage,
                      VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = srcStage;
        mb.srcAccessMask = srcAccess;
        mb.dstStageMask = dstStage;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    const VkPipelineStageFlags2 reads = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT;
    if (m_waterSimClear) {
        // (The contents are thrown away: from the undefined layout into the general one, which the field stays in.)
        VkImageMemoryBarrier2 ib[2] = {};
        for (int i = 0; i < 2; ++i) {
            ib[i].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2;
            ib[i].srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | reads;
            ib[i].dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
            ib[i].dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
            ib[i].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            ib[i].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            ib[i].image = m_waterSim[i];
            ib[i].subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        }
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = 2;
        dep.pImageMemoryBarriers = ib;
        vkCmdPipelineBarrier2(cmd, &dep);
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (int i = 0; i < 2; ++i)
            vkCmdClearColorImage(cmd, m_waterSim[i], VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        memory(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        m_waterSimClear = false;
    } else {
        // Last frame's step (and the water's reads of it) before this one.
        memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | reads,
               VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT,
               VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
    }

    // The characters, nearest to the camera first.
    struct Bodies {
        float pos[kSimBodies][4], dir[kSimBodies][4], extra[kSimBodies][4];
        float feet[kFootMax * 2][4];             // the falls' feet (last frame's: DrawWater finds them after this step)
        float footInfo[4];
    } bodies{};
    const size_t feet = std::min(m_waterFoot.size() / 8, kFootMax);
    if (P.fallChurn > 0.0f && feet) {
        std::memcpy(bodies.feet, m_waterFoot.data(), feet * 32);
        bodies.footInfo[0] = float(feet);
        bodies.footInfo[1] = 1.5f * P.fallChurn;
    }
    std::vector<uint32_t> order(m_waterBodies.size());
    for (uint32_t i = 0; i < uint32_t(order.size()); ++i)
        order[i] = i;
    const uint32_t n = uint32_t(std::min<size_t>(order.size(), kSimBodies));
    std::partial_sort(order.begin(), order.begin() + n, order.end(), [&](uint32_t a, uint32_t b) {
        const WaterBody& x = m_waterBodies[a], & y = m_waterBodies[b];
        float dx = x.x - eye[0], dz = x.z - eye[2], ex = y.x - eye[0], ez = y.z - eye[2];
        return dx * dx + dz * dz < ex * ex + ez * ez;
    });
    for (uint32_t i = 0; i < n; ++i) {
        const WaterBody& w = m_waterBodies[order[i]];
        bodies.pos[i][0] = w.x; bodies.pos[i][1] = w.z; bodies.pos[i][2] = w.minY; bodies.pos[i][3] = w.speed;
        const float inv = w.speed > 0.05f ? 1.0f / w.speed : 0.0f;
        bodies.dir[i][0] = w.vx * inv; bodies.dir[i][1] = w.vz * inv; bodies.dir[i][2] = w.phase;
        bodies.extra[i][0] = w.fall; bodies.extra[i][1] = w.prevMinY;
    }
    EnsureRingSpace(sizeof(Bodies) + 512);
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(Bodies), 256, &cpu);
    std::memcpy(cpu, &bodies, sizeof(Bodies));

    Frame& f = m_frames[m_frameIndex];
    const uint32_t cur = m_waterSimCur, next = cur ^ 1u;
    VkDescriptorImageInfo prevInfo{VK_NULL_HANDLE, m_waterSimView[cur], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo nextInfo{VK_NULL_HANDLE, m_waterSimView[next], VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo mapInfo{m_linearSampler, map->texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo bodiesInfo{f.ring, offset, sizeof(Bodies)};
    VkWriteDescriptorSet writes[4] = {};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
    }
    writes[0].descriptorType = writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[0].pImageInfo = &prevInfo;
    writes[1].pImageInfo = &nextInfo;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &mapInfo;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[3].pBufferInfo = &bodiesInfo;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_waterSimPipeline);
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_waterSimLayout, 0, 4, writes);
    // (The step stays within what the wave equation is stable for: the waves must cross less than ~0.6 of a cell.)
    const float dt = std::min(std::clamp(m_waterDt, 0.002f, 0.05f), 0.6f * cell / std::max(P.swimSpeed, 0.1f));
    const float strength = P.swimRipples;
    const float pc[16] = {
        eye[0], eye[2], cell, dt,
        map->origin[0], map->origin[1], 1.0f / map->size[0], 1.0f / map->size[1],
        map->base, float(n), float(std::fmod(m_waterSimTime, 3600.0)), 0.6f / std::max(P.swimLife, 0.05f),
        P.swimSpeed * P.swimSpeed, 5.0f * strength, 0.25f * strength * P.swimStir, P.swimSplash,
    };
    vkCmdPushConstants(cmd, m_waterSimLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), pc);
    vkCmdDispatch(cmd, kSimN / 8, kSimN / 8, 1);
    memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | reads,
           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_waterSimCur = next;
    m_waterSimLive = true;
}

// The scene's colour and depth as they are now, into the water's copies (once a frame: a second water body sees the
// first one's background, which is what it would see through it anyway).
bool Device::CopySceneForWater(VkCommandBuffer cmd, const WaterMap* map)
{
    if (m_waterCopyFrame == m_frameNumber)
        return true;
    const uint32_t w = m_scene->m_width, h = m_scene->m_height;
    if (!m_waterSceneCopy || m_waterSceneCopy->m_width != w || m_waterSceneCopy->m_height != h) {
        if (m_waterSceneCopy) DestroyTexture(m_waterSceneCopy);
        m_waterSceneCopy = CreateImage(w, h, Format::RGBA16F, 1, false);
        if (!m_waterSceneCopy)
            return false;
    }
    if (!m_waterDepthCopy || m_waterDepthW != w || m_waterDepthH != h) {
        if (m_waterDepthCopy)
            m_deadImages.push_back({DeathTag(), {m_waterDepthCopy, m_waterDepthCopyView, m_waterDepthCopyAllocation}});
        m_waterDepthCopy = VK_NULL_HANDLE;
        VkImageCreateInfo ci{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ci.imageType = VK_IMAGE_TYPE_2D;
        ci.format = kDepthFormat;
        ci.extent = {w, h, 1};
        ci.mipLevels = 1;
        ci.arrayLayers = 1;
        ci.samples = VK_SAMPLE_COUNT_1_BIT;
        ci.tiling = VK_IMAGE_TILING_OPTIMAL;
        ci.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        ac.flags = VMA_ALLOCATION_CREATE_DEDICATED_MEMORY_BIT;
        if (vmaCreateImage(m_allocator, &ci, &ac, &m_waterDepthCopy, &m_waterDepthCopyAllocation, nullptr) != VK_SUCCESS) {
            m_waterDepthCopy = VK_NULL_HANDLE;
            return false;
        }
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_waterDepthCopy;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = kDepthFormat;
        vi.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        vkCreateImageView(m_device, &vi, nullptr, &m_waterDepthCopyView);
        m_waterDepthW = w;
        m_waterDepthH = h;
        m_waterDepthCopyLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    m_renderEndCause = kEndCopy;
    EndRendering();
    Transition(cmd, m_scene, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    Transition(cmd, m_waterSceneCopy, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL);
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    ImageBarrier(cmd, m_waterDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, m_waterDepthCopyLayout,
                 VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT |
                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT, 0, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {w, h, 1};
    vkCmdCopyImage(cmd, m_scene->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_waterSceneCopy->m_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    region.srcSubresource.aspectMask = region.dstSubresource.aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
    vkCmdCopyImage(cmd, m_depth, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_waterDepthCopy,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Transition(cmd, m_waterSceneCopy, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    ImageBarrier(cmd, m_waterDepthCopy, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                 VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                 VK_ACCESS_2_TRANSFER_WRITE_BIT,
                 VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_waterDepthCopyLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    StepWaterSim(cmd, map);                      // (the render pass is closed: a compute pass fits here)
    BeginRenderingOn(m_target);
    m_waterCopyFrame = m_frameNumber;
    return true;
}

namespace {

// A vertex of the mesh as mesh mode reads it (water.vert MeshVertex, std430: 48 bytes): the game's position and
// colour, then for the steep triangles' vertices the fall's flow - a smooth normal over the whole sheet (the game's
// small triangles each lean their own way) and coordinates that run on unbroken across it (along: down from the
// fall's top; across: along its width, from its middle), so the streaks can slide down it with no seams.
struct MeshVertexOut {
    float pos[3];
    uint32_t colour;
    float normal[3];
    float along;
    float across;
    float fall;                                  // 1: a fall (no vertex alpha, a tall sheet); 0: a shore step
    float edge;                                  // a fall's sheet (BuildFallSheet): 0 at its sides, 1 well inside
    float pad;
};
static_assert(sizeof(MeshVertexOut) == 48, "water.vert MeshVertex");

// Each connected group of steep triangles (vertices welded by position: the game may split them along edges) is one
// fall: its face normals, turned to one side of the sheet and weighted by area, averaged at each vertex; its width
// the principal axis of its vertices across the ground (a curved fall is measured along a straight line: fine for
// the bends the game's falls have). shore: the steep triangles that are not falls (as vertex index triples); falls:
// each fall's triangles (as offsets into steep).
void BuildFallVertices(const Device::WaterVertex* v, uint32_t count, const std::vector<uint32_t>& steep,
                       MeshVertexOut* out, std::vector<uint32_t>& shore, std::vector<std::vector<uint32_t>>& fallTris)
{
    shore.clear();
    fallTris.clear();
    for (uint32_t i = 0; i < count; ++i) {
        std::memcpy(out[i].pos, v[i].pos, 12);
        out[i].colour = v[i].colour;
        out[i].normal[0] = out[i].normal[2] = 0.0f;
        out[i].normal[1] = 1.0f;
        out[i].along = out[i].across = out[i].fall = out[i].edge = out[i].pad = 0.0f;
    }
    if (steep.empty())
        return;
    // Weld: one id per position (to 1/64 unit).
    std::vector<uint32_t> weld(count, ~0u);
    std::vector<uint32_t> parent;                // union-find over the welded ids
    {
        std::vector<std::pair<uint64_t, uint32_t>> keys;
        keys.reserve(steep.size());
        for (uint32_t i : steep) {
            const auto q = [&](int k) { return uint64_t(int64_t(std::lround(v[i].pos[k] * 64.0f)) & 0x1FFFFF); };
            keys.emplace_back(q(0) | q(1) << 21 | q(2) << 42, i);
        }
        std::sort(keys.begin(), keys.end());
        for (size_t k = 0; k < keys.size(); ++k) {
            if (k == 0 || keys[k].first != keys[k - 1].first)
                parent.push_back(uint32_t(parent.size()));
            weld[keys[k].second] = uint32_t(parent.size() - 1);
        }
    }
    const auto find = [&](uint32_t a) {
        while (parent[a] != a) a = parent[a] = parent[parent[a]];
        return a;
    };
    for (size_t t = 0; t + 2 < steep.size(); t += 3) {
        const uint32_t a = find(weld[steep[t]]), b = find(weld[steep[t + 1]]), c = find(weld[steep[t + 2]]);
        parent[b] = a;
        parent[find(c)] = a;
    }
    // Per fall: its triangles' face normals (area-weighted) and the sums for its axes.
    struct Fall {
        double nxx = 0, nxz = 0, nzz = 0;        // the faces' horizontal normals' (unsigned) spread: which way it faces
        double sx = 0, sz = 0, sxx = 0, sxz = 0, szz = 0, n = 0;   // its vertices across the ground
        float top = -1e30f, bottom = 1e30f;
        uint32_t maxAlpha = 0;
        float face[2] = {1, 0}, axis[2] = {0, 1}, mid[2] = {0, 0};
    };
    std::vector<int32_t> fallOf(parent.size(), -1);
    std::vector<Fall> falls;
    const auto fallFor = [&](uint32_t i) -> Fall& {
        const uint32_t r = find(weld[i]);
        if (fallOf[r] < 0) { fallOf[r] = int32_t(falls.size()); falls.emplace_back(); }
        return falls[size_t(fallOf[r])];
    };
    std::vector<float> faceN(steep.size());      // each triangle's face normal x area (x, y, z per triangle)
    for (size_t t = 0; t + 2 < steep.size(); t += 3) {
        const float* p0 = v[steep[t]].pos;
        const float* p1 = v[steep[t + 1]].pos;
        const float* p2 = v[steep[t + 2]].pos;
        const float e1[3] = {p1[0] - p0[0], p1[1] - p0[1], p1[2] - p0[2]};
        const float e2[3] = {p2[0] - p0[0], p2[1] - p0[1], p2[2] - p0[2]};
        float* n = &faceN[t];
        n[0] = e1[1] * e2[2] - e1[2] * e2[1];
        n[1] = e1[2] * e2[0] - e1[0] * e2[2];
        n[2] = e1[0] * e2[1] - e1[1] * e2[0];
        Fall& f = fallFor(steep[t]);
        f.nxx += double(n[0]) * n[0];
        f.nxz += double(n[0]) * n[2];
        f.nzz += double(n[2]) * n[2];
    }
    std::vector<uint8_t> seen(count, 0);
    for (uint32_t i : steep) {
        Fall& f = fallFor(i);
        f.top = std::max(f.top, v[i].pos[1]);
        f.bottom = std::min(f.bottom, v[i].pos[1]);
        f.maxAlpha = std::max(f.maxAlpha, v[i].colour >> 24);
        if (seen[i]) continue;
        seen[i] = 1;
        const double x = v[i].pos[0], z = v[i].pos[2];
        f.sx += x; f.sz += z; f.n += 1;
        f.sxx += x * x; f.sxz += x * z; f.szz += z * z;
    }
    // The principal axis of a symmetric 2x2 [a b; b c].
    const auto principal = [](double a, double b, double c, float out2[2]) {
        const double angle = 0.5 * std::atan2(2.0 * b, a - c);
        out2[0] = float(std::cos(angle));
        out2[1] = float(std::sin(angle));
    };
    for (Fall& f : falls) {
        principal(f.nxx, f.nxz, f.nzz, f.face);
        f.mid[0] = float(f.sx / f.n);
        f.mid[1] = float(f.sz / f.n);
        const double cxx = f.sxx / f.n - double(f.mid[0]) * f.mid[0], cxz = f.sxz / f.n - double(f.mid[0]) * f.mid[1],
                     czz = f.szz / f.n - double(f.mid[1]) * f.mid[1];
        principal(cxx, cxz, czz, f.axis);
        if (f.axis[0] < 0.0f || (f.axis[0] == 0.0f && f.axis[1] < 0.0f)) { f.axis[0] = -f.axis[0]; f.axis[1] = -f.axis[1]; }
    }
    // The vertices' normals: the faces turned to the fall's facing side, summed.
    std::vector<float> sum(size_t(count) * 3, 0.0f);
    for (size_t t = 0; t + 2 < steep.size(); t += 3) {
        const Fall& f = fallFor(steep[t]);
        const float* n = &faceN[t];
        const float s = n[0] * f.face[0] + n[2] * f.face[1] < 0.0f ? -1.0f : 1.0f;
        for (int k = 0; k < 3; ++k)
            for (int j = 0; j < 3; ++j)
                sum[size_t(steep[t + k]) * 3 + j] += s * n[j];
    }
    // (Welded copies share their sums.)
    std::vector<float> weldSum(parent.size() * 3, 0.0f);
    for (uint32_t i : steep)
        if (seen[i] == 1) {
            seen[i] = 2;
            for (int j = 0; j < 3; ++j) weldSum[size_t(weld[i]) * 3 + j] += sum[size_t(i) * 3 + j];
        }
    for (uint32_t i : steep) {
        const Fall& f = fallFor(i);
        const float* s = &weldSum[size_t(weld[i]) * 3];
        const float l = std::sqrt(s[0] * s[0] + s[1] * s[1] + s[2] * s[2]);
        MeshVertexOut& o = out[i];
        if (l > 1e-12f)
            for (int j = 0; j < 3; ++j) o.normal[j] = s[j] / l;
        o.along = f.top - v[i].pos[1];
        o.across = (v[i].pos[0] - f.mid[0]) * f.axis[0] + (v[i].pos[2] - f.mid[1]) * f.axis[1];
        o.fall = f.maxAlpha == 0 && f.top - f.bottom > 3.0f ? 1.0f : 0.0f;
        o.edge = 1.0f;
    }
    std::vector<int32_t> slot(falls.size(), -1);
    for (size_t t = 0; t + 2 < steep.size(); t += 3) {
        if (out[steep[t]].fall < 0.5f) {
            shore.insert(shore.end(), {steep[t], steep[t + 1], steep[t + 2]});
            continue;
        }
        const size_t k = size_t(fallOf[find(weld[steep[t]])]);
        if (slot[k] < 0) { slot[k] = int32_t(fallTris.size()); fallTris.emplace_back(); }
        fallTris[size_t(slot[k])].push_back(uint32_t(t));
    }
}

// A fall's identity: its triangles' corners (quantised), in any order.
uint64_t FallKey(const MeshVertexOut* out, const std::vector<uint32_t>& steep, const std::vector<uint32_t>& tris)
{
    uint64_t key = tris.size();
    for (uint32_t t : tris)
        for (int k = 0; k < 3; ++k) {
            const float* p = out[steep[t + k]].pos;
            uint64_t h = uint64_t(uint32_t(std::lround(p[0] * 16.0f))) * 0x9E3779B97F4A7C15ull ^
                         uint64_t(uint32_t(std::lround(p[1] * 16.0f))) * 0xC2B2AE3D27D4EB4Full ^
                         uint64_t(uint32_t(std::lround(p[2] * 16.0f))) * 0x165667B19E3779F9ull;
            h ^= h >> 29;
            h *= 0xBF58476D1CE4E5B9ull;
            key += h ^ (h >> 32);
        }
    return key;
}

// The sheet we draw for a fall: its triangles resampled onto an even grid in its own flow coordinates (columns
// across it, rows down it, kSheetStep apart) - every vertex at the game's surface there, with the smooth normal - so
// the sheet can ripple (water.vert) and fade out at its sides (edge: by the distance to them, over kSheetFade; the
// top row comes over the lip and the bottom one goes into the pool: no fade there).
constexpr float kSheetStep = 0.5f, kSheetFade = 3.0f;
constexpr size_t kSheetMaxNodes = 300000;
// foot: where the sheet meets the pool, every kFootStep across it (Device::WaterFall::foot).
constexpr float kFootStep = 4.0f;
void BuildFallSheet(const MeshVertexOut* out, const std::vector<uint32_t>& steep, const std::vector<uint32_t>& tris,
                    std::vector<MeshVertexOut>& verts, std::vector<uint32_t>& indices, std::vector<float>& foot)
{
    verts.clear();
    indices.clear();
    foot.clear();
    float cLo = 1e30f, cHi = -1e30f, aHi = 0.0f;
    for (uint32_t t : tris)
        for (int k = 0; k < 3; ++k) {
            const MeshVertexOut& o = out[steep[t + k]];
            cLo = std::min(cLo, o.across);
            cHi = std::max(cHi, o.across);
            aHi = std::max(aHi, o.along);
        }
    if (cHi <= cLo || aHi <= 0.0f)
        return;
    float step = std::max(kSheetStep, std::sqrt((cHi - cLo) * aHi / float(kSheetMaxNodes)));
    const int nc = int(std::ceil((cHi - cLo) / step)) + 1, na = int(std::ceil(aHi / step)) + 2;
    std::vector<int32_t> node(size_t(nc) * size_t(na), -1);
    for (uint32_t t : tris) {
        const MeshVertexOut* o[3] = {&out[steep[t]], &out[steep[t + 1]], &out[steep[t + 2]]};
        float x[3], y[3];
        for (int k = 0; k < 3; ++k) { x[k] = (o[k]->across - cLo) / step; y[k] = o[k]->along / step; }
        const float det = (y[1] - y[2]) * (x[0] - x[2]) + (x[2] - x[1]) * (y[0] - y[2]);
        if (std::fabs(det) < 1e-8f)
            continue;                            // (edge-on in the flow coordinates: an overhang's fold)
        const int i0 = std::max(0, int(std::ceil(std::min({x[0], x[1], x[2]}) - 1e-3f)));
        const int i1 = std::min(nc - 1, int(std::floor(std::max({x[0], x[1], x[2]}) + 1e-3f)));
        const int j0 = std::max(0, int(std::ceil(std::min({y[0], y[1], y[2]}) - 1e-3f)));
        const int j1 = std::min(na - 1, int(std::floor(std::max({y[0], y[1], y[2]}) + 1e-3f)));
        for (int j = j0; j <= j1; ++j)
            for (int i = i0; i <= i1; ++i) {
                int32_t& slot = node[size_t(j) * nc + i];
                if (slot >= 0)
                    continue;
                const float w0 = ((y[1] - y[2]) * (float(i) - x[2]) + (x[2] - x[1]) * (float(j) - y[2])) / det;
                const float w1 = ((y[2] - y[0]) * (float(i) - x[2]) + (x[0] - x[2]) * (float(j) - y[2])) / det;
                const float w2 = 1.0f - w0 - w1;
                if (w0 < -1e-3f || w1 < -1e-3f || w2 < -1e-3f)
                    continue;
                MeshVertexOut m{};
                float n[3] = {};
                for (int c = 0; c < 3; ++c) {
                    m.pos[c] = o[0]->pos[c] * w0 + o[1]->pos[c] * w1 + o[2]->pos[c] * w2;
                    n[c] = o[0]->normal[c] * w0 + o[1]->normal[c] * w1 + o[2]->normal[c] * w2;
                }
                const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                for (int c = 0; c < 3; ++c) m.normal[c] = l > 1e-8f ? n[c] / l : (c == 1 ? 1.0f : 0.0f);
                m.along = float(j) * step;
                m.across = cLo + float(i) * step;
                m.fall = 1.0f;
                slot = int32_t(verts.size());
                verts.push_back(m);
            }
    }
    // The lip and the foot are not level, so a column's first node is mostly a little under the top row (and its last
    // above the bottom one): each column is carried on one node further at both ends (straight on, no higher than the
    // lip: top = y + along anywhere on the sheet), so the sheet meets the pool above and the one below with no gap.
    std::vector<int> first(nc, -1), last(nc, -1);
    for (int i = 0; i < nc; ++i) {
        for (int j = 0; j < na; ++j)
            if (node[size_t(j) * nc + i] >= 0) { if (first[i] < 0) first[i] = j; last[i] = j; }
        if (first[i] < 0 || last[i] == first[i])
            continue;
        for (int end = 0; end < 2; ++end) {
            const int j = end == 0 ? first[i] : last[i], dj = end == 0 ? -1 : 1;
            if (j + dj < 0 || j + dj >= na || node[size_t(j - dj) * nc + i] < 0)
                continue;
            const MeshVertexOut& a = verts[size_t(node[size_t(j) * nc + i])];
            const MeshVertexOut& b = verts[size_t(node[size_t(j - dj) * nc + i])];
            MeshVertexOut m = a;
            for (int c = 0; c < 3; ++c) m.pos[c] = 2.0f * a.pos[c] - b.pos[c];
            m.pos[1] = std::min(m.pos[1], a.pos[1] + a.along);
            m.along = float(j + dj) * step;
            node[size_t(j + dj) * nc + i] = int32_t(verts.size());
            verts.push_back(m);
            (end == 0 ? first[i] : last[i]) = j + dj;
        }
    }
    // The way out from the cliff: the water falls out as well as down, so from the lip over to the foot; an upright
    // sheet by its normals (facing out, a sheet leaning back looks up a little).
    float away[2] = {0.0f, 0.0f}, lean[2] = {0.0f, 0.0f};
    int columns = 0;
    for (int i = 0; i < nc; ++i) {
        if (first[i] < 0 || last[i] <= first[i])
            continue;
        const MeshVertexOut& a = verts[size_t(node[size_t(first[i]) * nc + i])];
        const MeshVertexOut& b = verts[size_t(node[size_t(last[i]) * nc + i])];
        away[0] += b.pos[0] - a.pos[0];
        away[1] += b.pos[2] - a.pos[2];
        const float sy = b.normal[1] >= 0.0f ? 1.0f : -1.0f;
        lean[0] += b.normal[0] * sy;
        lean[1] += b.normal[2] * sy;
        ++columns;
    }
    if (std::hypot(away[0], away[1]) < 0.5f * float(std::max(columns, 1))) { away[0] = lean[0]; away[1] = lean[1]; }
    const float al = std::hypot(away[0], away[1]);
    if (al > 1e-6f) { away[0] /= al; away[1] /= al; } else { away[0] = 1.0f; away[1] = 0.0f; }
    const int footEvery = std::max(1, int(std::lround(kFootStep / step)));
    for (int i = 0; i < nc; ++i) {
        if (last[i] < 0 || (i % footEvery != 0 && i != nc - 1))
            continue;
        const MeshVertexOut& b = verts[size_t(node[size_t(last[i]) * nc + i])];
        float o[2] = {b.normal[0], b.normal[2]};
        if (o[0] * away[0] + o[1] * away[1] < 0.0f) { o[0] = -o[0]; o[1] = -o[1]; }
        const float ol = std::hypot(o[0], o[1]);
        if (ol > 0.2f) { o[0] /= ol; o[1] /= ol; } else { o[0] = away[0]; o[1] = away[1]; }
        foot.insert(foot.end(), {b.pos[0], b.pos[1], b.pos[2], o[0], o[1]});
    }
    // The distance (in nodes) to the nearest empty one or a side: a chamfer transform, forward then back. (Empty above
    // a column's first node or below its last is over the lip or in the pool: not an edge.)
    constexpr float kFar = 1e9f;
    std::vector<float> d(node.size());
    for (int j = 0; j < na; ++j)
        for (int i = 0; i < nc; ++i) {
            const size_t k = size_t(j) * nc + i;
            d[k] = node[k] >= 0 || (first[i] >= 0 && (j < first[i] || j > last[i])) ? kFar : 0.0f;
        }
    const auto at = [&](int i, int j) {
        if (i < 0 || i >= nc) return 0.0f;       // beyond the sides: empty
        if (j < 0 || j >= na) return kFar;       // above the lip, below the pool: not an edge
        return d[size_t(j) * nc + i];
    };
    for (int j = 0; j < na; ++j)
        for (int i = 0; i < nc; ++i) {
            float& v = d[size_t(j) * nc + i];
            v = std::min({v, at(i - 1, j) + 1.0f, at(i, j - 1) + 1.0f, at(i - 1, j - 1) + 1.4142f, at(i + 1, j - 1) + 1.4142f});
        }
    for (int j = na - 1; j >= 0; --j)
        for (int i = nc - 1; i >= 0; --i) {
            float& v = d[size_t(j) * nc + i];
            v = std::min({v, at(i + 1, j) + 1.0f, at(i, j + 1) + 1.0f, at(i + 1, j + 1) + 1.4142f, at(i - 1, j + 1) + 1.4142f});
        }
    for (size_t k = 0; k < node.size(); ++k)
        if (node[k] >= 0)
            verts[size_t(node[k])].edge = std::clamp((d[k] - 1.0f) * step / kSheetFade, 0.0f, 1.0f);
    for (int j = 0; j + 1 < na; ++j)
        for (int i = 0; i + 1 < nc; ++i) {
            const int32_t a = node[size_t(j) * nc + i], b = node[size_t(j) * nc + i + 1];
            const int32_t c = node[size_t(j + 1) * nc + i], e = node[size_t(j + 1) * nc + i + 1];
            if (a < 0 || b < 0 || c < 0 || e < 0)
                continue;
            indices.insert(indices.end(), {uint32_t(a), uint32_t(c), uint32_t(b), uint32_t(b), uint32_t(c), uint32_t(e)});
        }
}

}  // namespace

void Device::DrawWater(const WaterVertex* vertices, uint32_t vertexCount, const uint16_t* indices, uint32_t indexCount)
{
    if (!m_inFrame || !vertexCount || indexCount < 3 || !m_waterParams.enable || !m_waterPipeline || !m_waterDetail ||
        !m_scenePhase || m_target != m_scene || !m_scene)
        return;
    ++m_frameDraw;
    std::vector<uint32_t>& steep = m_waterSteep;
    WaterMap* map = WaterMapFor(vertices, vertexCount, indices, indexCount, steep);
    if (!map && steep.empty())
        return;
    const WaterParams& P = m_waterParams;
    const uint32_t quality = std::clamp(P.quality, 1u, 3u);
    const uint32_t segments = quality == 1 ? 128u : quality == 2 ? 224u : 320u;
    const WaterGrid* grid = map ? WaterGridFor(segments) : nullptr;
    Frame& f = m_frames[m_frameIndex];
    VkCommandBuffer cmd = f.main;
    FlushGroup();                                // the scene's batched draws go first (and its profile class ends)
    ProfileSceneClass(8);
    if (!CopySceneForWater(cmd, map))
        return;

    // The block.
    const auto& vm = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    WaterFrame wf{};
    wf.viewProj = MulMatrix(m_view, m_proj);
    wf.prevViewProj = m_prevViewProjValid ? MulMatrix(m_prevView, m_prevProj) : wf.viewProj;
    wf.view = m_view;
    wf.proj[0] = m_proj.m[2][2];
    wf.proj[1] = m_proj.m[3][2];
    wf.proj[2] = m_proj.m[0][0];
    wf.proj[3] = m_proj.m[1][1];
    wf.camera[0] = eye[0];
    wf.camera[1] = eye[1];
    wf.camera[2] = eye[2];
    wf.camera[3] = TaaActive() ? 1.0f : 0.0f;
    wf.viewport[0] = float(m_scene->m_width);
    wf.viewport[1] = float(m_scene->m_height);
    wf.viewport[2] = 1.0f / wf.viewport[0];
    wf.viewport[3] = 1.0f / wf.viewport[1];
    wf.time[0] = float(m_windTime);
    wf.time[1] = float(m_windTimePrev);
    wf.time[3] = float(quality == 1 ? 16 : quality == 2 ? 28 : 44);
    static const int debugView = [] { const char* v = std::getenv("RANDYVK_WATER_DEBUG"); return v ? std::atoi(v) : 0; }();
    wf.sky[3] = float(debugView);
    float wind[4];
    Wind(wind);
    wf.wind[0] = wind[0];
    wf.wind[1] = wind[1];
    // Wave height: the setting, and the body's size - a pond stays calm, the open sea has a swell.
    float body = map ? std::clamp((map->extent - 60.0f) / 700.0f, 0.0f, 1.0f) : 0.0f;
    wf.wind[2] = P.waves * (0.12f + 0.88f * body * body);
    wf.wind[3] = 0.35f + 0.65f * body;
    if (m_terrainSun[0] + m_terrainSun[1] + m_terrainSun[2] > 0.0f) {
        std::memcpy(wf.sunColour, m_terrainSun, 16);
        std::memcpy(wf.sunDir, m_terrainSun + 4, 16);
    }
    if (m_terrainAmbient[3] > 0.5f)
        std::memcpy(wf.ambient, m_terrainAmbient, 16);
    else
        ArgbTo4(m_rs[d3d::RS_AMBIENT], wf.ambient);
    wf.look[0] = std::clamp(P.style, 0.0f, 1.0f);
    wf.look[1] = P.reflections;
    wf.look[2] = P.refraction;
    wf.look[3] = P.clarity;
    wf.look2[0] = P.foam;
    wf.look2[1] = P.caustics;
    wf.look2[2] = P.ripples;
    wf.look2[3] = m_textures[0] ? P.gameTexture : 0.0f;
    wf.fall[0] = P.fallSheen;
    wf.fall[1] = P.fallSparkle;
    ArgbTo4(m_rs[d3d::RS_FOGCOLOR], wf.fogColour);
    wf.fogColour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
    wf.fogParams[0] = AsFloatBits(m_rs[d3d::RS_FOGSTART]);
    wf.fogParams[1] = AsFloatBits(m_rs[d3d::RS_FOGEND]);
    wf.fogParams[2] = AsFloatBits(m_rs[d3d::RS_FOGDENSITY]);
    wf.fogParams[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
    // The environment probe (hdr.cpp RenderEnvProbe): what the reflections that miss the screen show, where it has
    // seen (else the sky).
    const bool env = m_envReady && m_envAtlas && m_pbr.enabled && m_pbr.probe > 0.0f;
    wf.env[0] = env ? 1.0f : 0.0f;
    wf.env[1] = m_pbr.probe;
    // The game's water colour; a colourless one (white vertices) gets a water blue.
    uint32_t colour = map ? map->colour : vertices[0].colour;
    ArgbTo4(colour, wf.tint);
    if (wf.tint[0] > 0.9f && wf.tint[1] > 0.9f && wf.tint[2] > 0.9f) {
        wf.tint[0] = 0.05f; wf.tint[1] = 0.2f; wf.tint[2] = 0.35f;
    }
    // The disturbance field the characters stir (StepWaterSim ran when the scene was copied): its normals and foam
    // (water.frag).
    wf.ripple[0] = m_waterSimLive ? 1.0f : 0.0f;
    wf.ripple[1] = P.swimGain;
    wf.ripple[2] = 1.0f / (m_waterSimCell * float(kSimN));
    wf.ripple[3] = m_waterSimCell;

    // The mesh's vertices with the falls' flow, the steep triangles that are not falls (drawn as they are), and each
    // fall's sheet (drawn instead of its triangles).
    static std::vector<MeshVertexOut> meshOut;
    static std::vector<uint32_t> shore;
    static std::vector<std::vector<uint32_t>> fallTris;
    meshOut.resize(vertexCount);
    BuildFallVertices(vertices, vertexCount, steep, meshOut.data(), shore, fallTris);
    // (Keys, not pointers, while m_waterFalls changes: adding or dropping a fall moves the others.)
    std::vector<uint64_t> sheetKeys;
    for (const std::vector<uint32_t>& tris : fallTris) {
        const uint64_t key = FallKey(meshOut.data(), steep, tris);
        auto it = std::find_if(m_waterFalls.begin(), m_waterFalls.end(), [&](const WaterFall& w) { return w.key == key; });
        if (it == m_waterFalls.end()) {
            if (m_waterFalls.size() >= 8) {
                // The one unseen longest goes - never one drawn this frame (all 8 are: this one isn't drawn).
                auto oldest = std::min_element(m_waterFalls.begin(), m_waterFalls.end(),
                                               [](const WaterFall& a, const WaterFall& b) { return a.lastFrame < b.lastFrame; });
                if (oldest->lastFrame == m_frameNumber)
                    continue;
                m_waterFalls.erase(oldest);
            }
            std::vector<MeshVertexOut> sv;
            WaterFall w;
            w.key = key;
            BuildFallSheet(meshOut.data(), steep, tris, sv, w.indices, w.foot);
            w.vertices.resize(sv.size() * 12);
            std::memcpy(w.vertices.data(), sv.data(), sv.size() * sizeof(MeshVertexOut));
            WaterLog("water fall %016llx: %zu triangles -> sheet of %zu vertices, %zu triangles, %zu foot points",
                     (unsigned long long)key, tris.size(), sv.size(), w.indices.size() / 3, w.foot.size() / 5);
            m_waterFalls.push_back(std::move(w));
            it = m_waterFalls.end() - 1;
        }
        it->lastFrame = m_frameNumber;
        if (!it->indices.empty())
            sheetKeys.push_back(key);
    }
    std::vector<const WaterFall*> sheets;
    VkDeviceSize sheetBytes = 0;
    for (const WaterFall& w : m_waterFalls)
        if (std::find(sheetKeys.begin(), sheetKeys.end(), w.key) != sheetKeys.end()) {
            sheets.push_back(&w);
            sheetBytes += w.vertices.size() * 4 + w.indices.size() * 4 + 512;
        }
    // Their feet, as segments (FootSegment) between neighbouring foot points - the nearest kFootMax to the camera.
    m_waterFoot.clear();
    for (const WaterFall* w : sheets)
        for (size_t k = 5; k < w->foot.size(); k += 5) {
            const float* a = &w->foot[k - 5];
            const float* b = &w->foot[k];
            if (std::hypot(b[0] - a[0], b[2] - a[2]) > 3.0f * kFootStep)
                continue;                        // (a gap in the sheet)
            float o[2] = {a[3] + b[3], a[4] + b[4]};
            const float ol = std::max(std::hypot(o[0], o[1]), 1e-6f);
            m_waterFoot.insert(m_waterFoot.end(), {a[0], a[1], a[2], b[0], b[1], b[2], o[0] / ol, o[1] / ol});
        }
    if (m_waterFoot.size() > kFootMax * 8) {
        std::vector<std::pair<float, size_t>> byDistance;
        for (size_t k = 0; k < m_waterFoot.size(); k += 8)
            byDistance.emplace_back(std::hypot((m_waterFoot[k] + m_waterFoot[k + 3]) * 0.5f - eye[0],
                                            (m_waterFoot[k + 2] + m_waterFoot[k + 5]) * 0.5f - eye[2]), k);
        std::partial_sort(byDistance.begin(), byDistance.begin() + kFootMax, byDistance.end());
        std::vector<float> kept;
        for (size_t k = 0; k < kFootMax; ++k)
            kept.insert(kept.end(), m_waterFoot.begin() + ptrdiff_t(byDistance[k].second), m_waterFoot.begin() + ptrdiff_t(byDistance[k].second + 8));
        m_waterFoot.swap(kept);
    }
    const uint32_t footCount = uint32_t(m_waterFoot.size() / 8);
    wf.fall[2] = float(footCount);
    wf.fall2[0] = P.fallSpray;
    wf.fall2[1] = P.fallChurn;
    wf.fall2[2] = P.fallSpeed;
    wf.fall2[3] = m_rain;                        // (last frame's: the rain is worked out at the scene's end)
    EnsureRingSpace(4 * (sizeof(WaterFrame) + 256) + sizeof(FrameLights) + VkDeviceSize(vertexCount) * sizeof(MeshVertexOut) +
                    shore.size() * 4 + sheetBytes + kFootMax * 32 + 1024);
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    void* meshCpu = nullptr;
    const VkDeviceSize meshBytes = std::max<VkDeviceSize>(VkDeviceSize(vertexCount) * sizeof(MeshVertexOut), 16);
    const VkDeviceSize meshOffset = Allocate(meshBytes, 256, &meshCpu);
    std::memcpy(meshCpu, meshOut.data(), size_t(vertexCount) * sizeof(MeshVertexOut));
    // The feet (the grid's foam and the spray read them where the mesh mode reads its vertices).
    void* footCpu = nullptr;
    const VkDeviceSize footBytes = std::max<VkDeviceSize>(VkDeviceSize(footCount) * 32, 32);
    const VkDeviceSize footOffset = Allocate(footBytes, 256, &footCpu);
    std::memset(footCpu, 0, size_t(footBytes));
    if (footCount)
        std::memcpy(footCpu, m_waterFoot.data(), size_t(footCount) * 32);

    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};   // the scene draws into a flipped viewport
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterPipeline);

    // kind 1, mesh mode: vertices from meshAt (meshSize bytes), indices idx (idxCount); 0: the grid; 2: the spray
    // (idxCount vertices, no indices).
    auto draw = [&](int kind, VkDeviceSize meshAt, VkDeviceSize meshSize, const uint32_t* idx, uint32_t idxCount) {
        const bool meshMode = kind == 1;
        WaterFrame block = wf;
        block.time[2] = meshMode ? 1.0f : 0.0f;
        if (!meshMode && map) {
            block.map[0] = map->origin[0];
            block.map[1] = map->origin[1];
            block.map[2] = 1.0f / map->size[0];
            block.map[3] = 1.0f / map->size[1];
            block.mapInfo[0] = map->base;
            block.mapInfo[1] = map->texel;
            block.mapInfo[2] = 1.0f;
        }
        const float ratio = 1.0f + 6.2831853f / float(segments);
        block.grid[0] = float(segments);
        block.grid[2] = kGridFirstRing;
        block.grid[3] = ratio;
        uint32_t rings = 0;
        if (!meshMode && map) {
            // Out to the map's farthest corner (or the fog's end, past which nothing shows).
            float reach = 0.0f;
            for (float x : {map->lo[0], map->hi[0]})
                for (float z : {map->lo[1], map->hi[1]})
                    reach = std::max(reach, std::hypot(x - eye[0], z - eye[2]));
            if (block.fogColour[3] > 0.5f && block.fogParams[3] == 3.0f && block.fogParams[1] > 0.0f)
                reach = std::min(reach, block.fogParams[1] * 1.05f);
            reach = std::clamp(reach, 1.0f, 20000.0f);
            rings = std::min(uint32_t(std::ceil(std::log(reach / kGridFirstRing) / std::log(ratio))) + 2, kGridMaxRings - 1);
            block.grid[1] = float(rings);
            if (m_frameNumber < 4)
                WaterLog("water grid: reach %.1f, %u rings x %u segments, eye %.1f %.1f %.1f", double(reach), rings,
                         segments, double(eye[0]), double(eye[1]), double(eye[2]));
        }
        void* blockCpu = nullptr;
        const VkDeviceSize blockOffset = Allocate(sizeof(WaterFrame), 256, &blockCpu);
        std::memcpy(blockCpu, &block, sizeof(block));
        Texture* mapTex = (!meshMode && map) ? map->texture : m_waterDetail;
        Texture* gameTex = m_textures[0] ? m_textures[0] : m_waterDetail;
        VkDescriptorBufferInfo blockInfo{f.ring, blockOffset, sizeof(WaterFrame)};
        VkDescriptorBufferInfo meshInfo{f.ring, meshAt, meshSize};
        VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorImageInfo shadowInfo{m_shadowSampler, m_shadowView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo mapInfo{m_linearSampler, mapTex->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo depthInfo{m_pointSampler, m_waterDepthCopyView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sceneInfo{m_linearSampler, m_waterSceneCopy->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo detailInfo{m_waterRepeatSampler, m_waterDetail->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo gameInfo{m_waterRepeatSampler, gameTex->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo envInfo{m_linearSampler, (env ? m_envAtlas : m_waterDetail)->m_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo simInfo{m_waterRepeatSampler, m_waterSimLive ? m_waterSimView[m_waterSimCur] : m_waterDetail->m_view,
                                      m_waterSimLive ? VK_IMAGE_LAYOUT_GENERAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo cloudInfo{m_linearSampler, CloudShadowView(), VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        const bool terrainMap = m_terrainSunMap && m_terrainSunMap->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo terrainInfo{m_linearSampler, terrainMap ? m_terrainSunMap->m_view : m_blackTexture->m_view,
                                          VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet writes[13] = {};
        const uint32_t bindings[13] = {0, 2, 4, 5, 7, 8, 9, 10, 11, 12, 13, 14, 15};
        for (int i = 0; i < 13; ++i) {
            writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            writes[i].descriptorCount = 1;
            writes[i].dstBinding = bindings[i];
            writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        writes[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[0].pBufferInfo = &blockInfo;
        writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        writes[1].pBufferInfo = &meshInfo;
        writes[2].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[2].pBufferInfo = &lightsInfo;
        writes[3].pImageInfo = &shadowInfo;
        writes[4].pImageInfo = &mapInfo;
        writes[5].pImageInfo = &depthInfo;
        writes[6].pImageInfo = &sceneInfo;
        writes[7].pImageInfo = &detailInfo;
        writes[8].pImageInfo = &gameInfo;
        writes[9].pImageInfo = &envInfo;
        writes[10].pImageInfo = &simInfo;
        writes[11].pImageInfo = &cloudInfo;
        writes[12].pImageInfo = &terrainInfo;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterLayout, 0, 13, writes);
        if (kind == 2) {
            vkCmdDraw(cmd, idxCount, 1, 0, 0);
        } else if (meshMode) {
            void* idxCpu = nullptr;
            const VkDeviceSize idxOffset = Allocate(VkDeviceSize(idxCount) * 4, 16, &idxCpu);
            std::memcpy(idxCpu, idx, size_t(idxCount) * 4);
            vkCmdBindIndexBuffer(cmd, f.ring, idxOffset, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, idxCount, 1, 0, 0, 0);
            m_waterTriangles += idxCount / 3;
        } else {
            vkCmdBindIndexBuffer(cmd, grid->buffer, 0, VK_INDEX_TYPE_UINT32);
            vkCmdDrawIndexed(cmd, rings * segments * 6, 1, 0, 0, 0);
            m_waterTriangles += uint64_t(rings) * segments * 2;
        }
    };
    if (map && grid)
        draw(0, footOffset, footBytes, nullptr, 0);
    if (!shore.empty())
        draw(1, meshOffset, meshBytes, shore.data(), uint32_t(shore.size()));
    for (const WaterFall* w : sheets) {
        void* cpu = nullptr;
        const VkDeviceSize bytes = w->vertices.size() * 4;
        const VkDeviceSize at = Allocate(bytes, 256, &cpu);
        std::memcpy(cpu, w->vertices.data(), size_t(bytes));
        draw(1, at, bytes, w->indices.data(), uint32_t(w->indices.size()));
    }
    // The spray and mist at the falls' feet: soft particles over everything drawn (water_spray.*).
    if (footCount && P.fallSpray > 0.0f && m_waterSprayPipeline) {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_waterSprayPipeline);
        draw(2, footOffset, footBytes, nullptr, footCount * kSprayPerFoot * 6);
    }
    m_cache = StateCache{};                      // this pipeline is not the scene's
    m_arenaBound = m_bindlessBound = false;      // ... and its pushed set 0 replaced the scene's: rebind those too
    ProfileMark("water");
    ++m_waterDraws;
}

}  // namespace rvk
