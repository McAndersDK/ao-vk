// Enhancement: smoke (RVK_SmokeOn). Each smoke source handed over (SmokeSourceFrame: where, how much, how hot, its
// colour...) gets a box of N^3 cells round it - standing on it, the source near its floor - in which a fluid is
// simulated on the GPU each frame (shaders/smoke_sim.comp: velocity moved along itself with buoyancy, the wind and the
// source's push; vorticity confinement; a pressure solve keeping it incompressible; density and temperature moved
// along it; characters stirring it; the sunlight through it), then ray-marched over the finished scene (smoke.vert/.frag), before the rain.
//
// A box follows its source; its contents stay where they are in the world (the simulation reads them shifted by the
// box's move). A new volume warms up: its first kWarmUp seconds simulated kWarmSpeed times as fast while it fades in,
// so a source coming into view shows a grown plume, not one growing from nothing. Sources not handed over any more go on until their smoke has faded (about their life x 1.5); the
// volumes are kept for the next. The velocity and the density are kept in pairs per volume (one read, one written,
// swapped each step); the curl, the divergence and the pressure are scratch, shared by all.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kSmokeSimSpirv[] = {
#include "smoke_sim.comp.inc"
};
const uint32_t kSmokeVertSpirv[] = {
#include "smoke.vert.inc"
};
const uint32_t kSmokeFragSpirv[] = {
#include "smoke.frag.inc"
};

constexpr uint32_t kSmokeMax = 8;               // volumes
constexpr uint32_t kSmokeSimMax = 6;            // ... simulated a frame (the nearest)
constexpr float kSmokeRange = 160.0f;           // simulated and drawn this near only
constexpr uint32_t kJacobi = 24;                // pressure steps
constexpr float kWarmUp = 2.0f, kWarmSpeed = 3.0f;
constexpr uint32_t kNoiseSize = 64;             // the wisps' noise image (cells a side)

constexpr uint32_t kSmokeBodies = 8;            // characters stirring a volume (smoke_sim.comp Bodies)
constexpr float kBodyRadius = 0.35f;            // a character's (world)

// The characters in a volume (smoke_sim.comp Bodies, std140).
struct SmokeBodies {
    float info[4];
    float pos[kSmokeBodies][4], vel[kSmokeBodies][4];
};

constexpr float kWindScale = 0.1f, kWindMax = 0.8f;    // of the game's wind (its sprites drift far less than it says)

// The wind a source's smoke is carried by (world / s, x z).
void SmokeWind(const Device::SmokeSource& s, const float rain[3], float out[2])
{
    out[0] = (s.wind[0] + rain[0]) * kWindScale;
    out[1] = (s.wind[2] + rain[2]) * kWindScale;
    const float l = std::hypot(out[0], out[1]);
    if (l > kWindMax) { out[0] *= kWindMax / l; out[1] *= kWindMax / l; }
}

// A source's box: standing on it, the source near its floor - and upwind of the middle, as far as the wind blows
// it (the plume leans downwind, in the box).
void SmokeCorner(const Device::SmokeSource& s, const float wind[2], float out[3])
{
    out[0] = s.pos[0] - s.size * (0.5f - 0.3f * wind[0] / kWindMax);
    out[1] = s.pos[1] - s.size * 0.08f;
    out[2] = s.pos[2] - s.size * (0.5f - 0.3f * wind[1] / kWindMax);
}

// How hard a source puffs now (0.2..1): uneven, so the smoke rises in billows, not as a steady column.
float Puff(double t, uint64_t key)
{
    const double seed = double(key % 997u) * 0.37;
    const double x = 0.55 + 0.25 * std::sin(t * 2.3 + seed) + 0.2 * std::sin(t * 3.7 + seed * 1.9) +
                     0.12 * std::sin(t * 6.1 + seed * 0.7);
    return float(std::clamp(x, 0.2, 1.0));
}

struct SimPush {
    float a[4], b[4], c[4], d[4], e[4], f[4];   // smoke_sim.comp Params
};
static_assert(sizeof(SimPush) == 96, "push constants");

// The per-draw block (smoke_common.glsl SmokeFrame, std140).
struct SmokeFrame {
    d3d::Matrix viewProj, view;
    float camera[4], box[4], colour[4], ambient[4], proj[4], viewport[4], fogColour[4], fogParams[4], detail[4];
};

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

}  // namespace

void Device::SmokeSourceFrame(uint64_t key, const SmokeSource& source)
{
    if (!key || !m_smokeParams.enable)
        return;
    SmokeVolume* v = nullptr;
    for (SmokeVolume& x : m_smokeVolumes)
        if (x.key == key) { v = &x; break; }
    if (!v) {
        for (SmokeVolume& x : m_smokeVolumes)
            if (!x.key) { v = &x; break; }
        if (!v && m_smokeVolumes.size() < kSmokeMax) {
            m_smokeVolumes.emplace_back();
            v = &m_smokeVolumes.back();
        }
        if (!v) {
            // All taken: the one gone longest, if it is gone at all; else none for this source.
            SmokeVolume* oldest = nullptr;
            for (SmokeVolume& x : m_smokeVolumes)
                if (x.lastSeen < m_frameClock - 0.5 && (!oldest || x.lastSeen < oldest->lastSeen)) oldest = &x;
            if (!oldest)
                return;
            v = oldest;
        }
        v->key = key;
        v->fresh = true;
        v->age = 0.0f;
        float wind[2];
        SmokeWind(source, m_rainWind, wind);
        SmokeCorner(source, wind, v->corner);
    }
    v->source = source;
    v->lastSeen = m_frameClock;
}

bool Device::MakeSmokeImage(VkFormat format, uint32_t n, SmokeImage& out)
{
    VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ic.imageType = VK_IMAGE_TYPE_3D;
    ic.format = format;
    ic.extent = {n, n, n};
    ic.mipLevels = 1;
    ic.arrayLayers = 1;
    ic.samples = VK_SAMPLE_COUNT_1_BIT;
    ic.tiling = VK_IMAGE_TILING_OPTIMAL;
    ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    VmaAllocationCreateInfo ac{};
    ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
    std::string error;
    if (!Check(vmaCreateImage(m_allocator, &ic, &ac, &out.image, &out.allocation, nullptr), "smoke image", &error))
        return false;
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = out.image;
    vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
    vi.format = format;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    return Check(vkCreateImageView(m_device, &vi, nullptr, &out.view), "smoke view", &error);
}

void Device::DestroySmokeImages()
{
    auto destroy = [&](SmokeImage& i) {
        if (i.view) vkDestroyImageView(m_device, i.view, nullptr);
        if (i.image) vmaDestroyImage(m_allocator, i.image, i.allocation);
        i = SmokeImage{};
    };
    for (SmokeVolume& v : m_smokeVolumes) {
        for (int k = 0; k < 2; ++k) { destroy(v.vel[k]); destroy(v.dens[k]); }
        destroy(v.light);
        v.fresh = true;
        v.age = 0.0f;
    }
    destroy(m_smokeCurl);
    destroy(m_smokeNoise);
    m_smokeNoiseMade = false;
    destroy(m_smokeDiv);
    destroy(m_smokePressure[0]);
    destroy(m_smokePressure[1]);
    m_smokeSharedFresh = true;
    m_smokeN = 0;
}

bool Device::CreateSmokeResources(std::string* error)
{
    DestroySmokeResources();
    // The sampler: filtered, nothing beyond the box (black border).
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
    si.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_smokeSampler), "smoke sampler", error))
        return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;   // (the wisps' noise)
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_smokeRepeatSampler), "smoke sampler", error))
        return false;
    // The simulation: 0, 1, 10 sampled; 2..9 storage images; 11 the characters.
    {
        VkDescriptorSetLayoutBinding b[12] = {};
        for (uint32_t i = 0; i < 12; ++i)
            b[i] = {i, i < 2 || i == 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 12;
        sl.pBindings = b;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_smokeSimSetLayout), "smoke sim set", error))
            return false;
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SimPush)};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &m_smokeSimSetLayout;
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_smokeSimLayout), "smoke sim layout", error))
            return false;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(kSmokeSimSpirv);
        mi.pCode = kSmokeSimSpirv;
        VkShaderModule module;
        if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "smoke sim module", error))
            return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                    "main", nullptr};
        ci.layout = m_smokeSimLayout;
        bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_smokeSimPipeline),
                        "smoke sim pipeline", error);
        vkDestroyShaderModule(m_device, module, nullptr);
        if (!ok)
            return false;
    }
    // The rendering: 0 the block, 1 the density, 2 the sunlight, 3 the scene's depth, 4 the frame lights, 5 the flow,
    // 6 the wisps' noise.
    VkDescriptorSetLayoutBinding b[7] = {};
    for (uint32_t i = 0; i < 7; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr};
    b[0].descriptorType = b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 7;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_smokeSetLayout), "smoke set", error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_smokeSetLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_smokeLayout), "smoke layout", error))
        return false;
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "smoke module", error);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = module(kSmokeVertSpirv, sizeof(kSmokeVertSpirv), &vert) && module(kSmokeFragSpirv, sizeof(kSmokeFragSpirv), &frag);
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
        rs.cullMode = VK_CULL_MODE_NONE;         // (smoke.frag keeps the far faces)
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        // Premultiplied over every target alike (no independent blending); the shader gives the others nothing.
        VkPipelineColorBlendAttachmentState att[5] = {};
        for (auto& a : att) {
            a.colorWriteMask = 0xF;
            a.blendEnable = VK_TRUE;
            a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
            a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
            a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
        }
        VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        cb.attachmentCount = 5;
        cb.pAttachments = att;
        VkFormat colorFormats[5] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA16F).vk,
                                    GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RGBA16F).vk,
                                    GetFormatInfo(Format::A8R8G8B8).vk};
        VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
        rendering.colorAttachmentCount = 5;
        rendering.pColorAttachmentFormats = colorFormats;
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
        ci.layout = m_smokeLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_smokePipeline),
                   "vkCreateGraphicsPipelines (smoke)", error);
    }
    if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
    if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void Device::DestroySmokeResources()
{
    DestroySmokeImages();
    m_smokeVolumes.clear();
    if (m_smokePipeline) { vkDestroyPipeline(m_device, m_smokePipeline, nullptr); m_smokePipeline = VK_NULL_HANDLE; }
    if (m_smokeSimPipeline) { vkDestroyPipeline(m_device, m_smokeSimPipeline, nullptr); m_smokeSimPipeline = VK_NULL_HANDLE; }
    if (m_smokeLayout) { vkDestroyPipelineLayout(m_device, m_smokeLayout, nullptr); m_smokeLayout = VK_NULL_HANDLE; }
    if (m_smokeSimLayout) { vkDestroyPipelineLayout(m_device, m_smokeSimLayout, nullptr); m_smokeSimLayout = VK_NULL_HANDLE; }
    if (m_smokeSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_smokeSetLayout, nullptr); m_smokeSetLayout = VK_NULL_HANDLE; }
    if (m_smokeSimSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_smokeSimSetLayout, nullptr); m_smokeSimSetLayout = VK_NULL_HANDLE; }
    if (m_smokeSampler) { vkDestroySampler(m_device, m_smokeSampler, nullptr); m_smokeSampler = VK_NULL_HANDLE; }
    if (m_smokeRepeatSampler) { vkDestroySampler(m_device, m_smokeRepeatSampler, nullptr); m_smokeRepeatSampler = VK_NULL_HANDLE; }
}

// The smoke: simulated, then drawn over the finished scene (EndScene: its rendering just closed).
void Device::RenderSmoke(VkCommandBuffer cmd)
{
    const SmokeParams& P = m_smokeParams;
    const double now = m_frameClock;
    const float dt = m_smokeLastClock < 0.0 ? 0.0f : float(std::clamp(now - m_smokeLastClock, 0.0, 1.0 / 30.0));
    m_smokeLastClock = now;
    if (!P.enable || !m_smokeSimPipeline || !m_smokePipeline || !m_scene || !m_hdr || m_smokeVolumes.empty())
        return;
    // Gone sources whose smoke has faded give their volumes back.
    for (SmokeVolume& v : m_smokeVolumes)
        if (v.key && now - v.lastSeen > double(v.source.life) * 1.5 + 1.0)
            v.key = 0;
    const uint32_t n = P.quality <= 1 ? 48u : P.quality == 2 ? 64u : 80u;
    if (n != m_smokeN) {
        DestroySmokeImages();
        m_smokeN = n;
    }
    const auto& vm = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    // The nearest live ones, up to kSmokeSimMax, within range.
    std::vector<std::pair<float, SmokeVolume*>> live;
    for (SmokeVolume& v : m_smokeVolumes) {
        if (!v.key)
            continue;
        const float d = std::hypot(v.source.pos[0] - eye[0], v.source.pos[1] - eye[1], v.source.pos[2] - eye[2]);
        if (d < kSmokeRange + v.source.size)
            live.emplace_back(d, &v);
    }
    std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
    if (live.size() > kSmokeSimMax)
        live.resize(kSmokeSimMax);
    if (live.empty())
        return;

    // Images: the shared scratch, and each volume's on first use.
    const VkFormat rgba = VK_FORMAT_R16G16B16A16_SFLOAT, r32 = VK_FORMAT_R32_SFLOAT, r16 = VK_FORMAT_R16_SFLOAT;
    std::vector<VkImageMemoryBarrier2> toGeneral;
    std::vector<VkImage> toClear;
    auto ensure = [&](SmokeImage& img, VkFormat f, bool clear) {
        if (img.image)
            return true;
        if (!MakeSmokeImage(f, n, img))
            return false;
        VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
        b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
        b.dstStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
        b.dstAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
        b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
        b.image = img.image;
        b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        toGeneral.push_back(b);
        if (clear) toClear.push_back(img.image);
        return true;
    };
    bool ok = ensure(m_smokeCurl, rgba, false) && ensure(m_smokeDiv, r32, false) && ensure(m_smokePressure[0], r32, false) &&
              ensure(m_smokePressure[1], r32, false);
    if (ok && !m_smokeNoise.image) {             // (its own size, not the grid's)
        ok = MakeSmokeImage(r16, kNoiseSize, m_smokeNoise);
        if (ok) {
            VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
            b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
            b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
            b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
            b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
            b.image = m_smokeNoise.image;
            b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
            toGeneral.push_back(b);
        }
    }
    for (auto& [d, v] : live) {
        ok = ok && ensure(v->vel[0], rgba, true) && ensure(v->vel[1], rgba, true) && ensure(v->dens[0], rgba, true) &&
             ensure(v->dens[1], rgba, true) && ensure(v->light, r16, false);
        if (v->fresh)
            for (int k = 0; k < 2; ++k) { toClear.push_back(v->vel[k].image); toClear.push_back(v->dens[k].image); }
        v->fresh = false;
    }
    if (!ok) {
        Log("smoke: images unavailable");
        m_smokeParams.enable = false;
        return;
    }
    auto memory = [&](VkPipelineStageFlags2 src, VkAccessFlags2 srcAccess, VkPipelineStageFlags2 dst, VkAccessFlags2 dstAccess) {
        VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
        mb.srcStageMask = src;
        mb.srcAccessMask = srcAccess;
        mb.dstStageMask = dst;
        mb.dstAccessMask = dstAccess;
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.memoryBarrierCount = 1;
        dep.pMemoryBarriers = &mb;
        vkCmdPipelineBarrier2(cmd, &dep);
    };
    // Last frame's drawing (fragment reads) before this frame's simulation writes.
    memory(VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
           VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_2_TRANSFER_BIT,
           VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT | VK_ACCESS_2_TRANSFER_WRITE_BIT);
    if (!toGeneral.empty()) {
        VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
        dep.imageMemoryBarrierCount = uint32_t(toGeneral.size());
        dep.pImageMemoryBarriers = toGeneral.data();
        vkCmdPipelineBarrier2(cmd, &dep);
    }
    if (!toClear.empty()) {
        VkClearColorValue zero{};
        VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        for (VkImage i : toClear)
            vkCmdClearColorImage(cmd, i, VK_IMAGE_LAYOUT_GENERAL, &zero, 1, &range);
        memory(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    }

    // The sun (towards it) and the wind (the rain's, from the game's weather; else none).
    float sun[3] = {-m_frameSunDir[0], -m_frameSunDir[1], -m_frameSunDir[2]};
    const float sl = std::max(std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]), 1e-6f);
    for (float& s : sun) s /= sl;
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_smokeSimPipeline);
    uint32_t groups = n / 4;
    EnsureRingSpace(live.size() * (sizeof(SmokeBodies) + 256) + 1024);
    Frame& f = m_frames[m_frameIndex];
    for (auto& [dist, vp] : live) {
        SmokeVolume& v = *vp;
        const SmokeSource& s = v.source;
        const bool fed = now - v.lastSeen < 0.25;           // a source gone: its smoke goes on, nothing added
        float corner[3], shift[3], wind[2];
        SmokeWind(s, m_rainWind, wind);
        SmokeCorner(s, wind, corner);
        for (int i = 0; i < 3; ++i) {
            shift[i] = (corner[i] - v.corner[i]) / s.size;
            v.corner[i] = corner[i];
        }
        const bool warming = v.age < kWarmUp;
        const float step = warming ? dt * kWarmSpeed : dt;
        v.age += dt;
        SimPush p{};
        p.a[1] = float(n);
        p.a[2] = step;
        p.a[3] = s.size;
        for (int i = 0; i < 3; ++i)
            p.b[i] = (s.pos[i] - corner[i]) / s.size;
        p.b[3] = s.radius / s.size;
        const float puff = Puff(now, v.key);
        // (A small source denser, so it gives about as much smoke; RVK_SmokeDensity is the extinction's, not this.)
        const float small = std::clamp(0.5f / std::max(s.radius, 0.05f), 1.0f, 3.0f);
        p.c[0] = fed ? s.rate * small * (0.5f + 0.5f * puff) : 0.0f;
        p.c[1] = fed ? s.heat * (0.6f + 0.8f * puff) : 0.0f;
        p.c[2] = fed ? s.speed * (0.4f + 1.0f * puff) : 0.0f;
        p.c[3] = 2.0f / std::max(s.life, 0.3f);            // (the game's sprites fade out over their life)
        p.d[0] = 2.5f;
        p.d[1] = 18.0f;
        p.d[2] = wind[0];
        p.d[3] = wind[1];
        std::memcpy(p.e, shift, sizeof(shift));
        std::memcpy(p.f, sun, sizeof(sun));
        p.f[3] = 1.6f * P.density;                      // (as thick to the sun as it is to the eye)
        // The characters in the box (last frame's, moved on by how they went), nearest the source first.
        SmokeBodies bodies{};
        bodies.info[1] = P.stir;
        if (P.stir > 0.0f) {
            const float ahead = float(std::clamp(now - m_waterBodiesTime, 0.0, 0.1));
            std::vector<std::pair<float, const WaterBody*>> in;
            for (const WaterBody& w : m_waterBodies) {
                const float top = 2.0f * w.centreY - w.minY;
                const float x = (w.x - corner[0]) / s.size, z = (w.z - corner[2]) / s.size;
                if (x < -0.1f || x > 1.1f || z < -0.1f || z > 1.1f || top < corner[1] || w.minY > corner[1] + s.size)
                    continue;
                in.emplace_back(std::hypot(w.x - s.pos[0], w.z - s.pos[2]), &w);
            }
            std::sort(in.begin(), in.end(), [](const auto& a, const auto& b) { return a.first < b.first; });
            const uint32_t count = uint32_t(std::min<size_t>(in.size(), kSmokeBodies));
            for (uint32_t i = 0; i < count; ++i) {
                const WaterBody& w = *in[i].second;
                const float vy = std::clamp(-w.fall, -10.0f, 10.0f);
                const float top = 2.0f * w.centreY - w.minY;
                bodies.pos[i][0] = (w.x + w.vx * ahead - corner[0]) / s.size;
                bodies.pos[i][1] = (w.minY + vy * ahead - corner[1]) / s.size;
                bodies.pos[i][2] = (w.z + w.vz * ahead - corner[2]) / s.size;
                bodies.pos[i][3] = (top + vy * ahead - corner[1]) / s.size;
                bodies.vel[i][0] = w.vx;
                bodies.vel[i][1] = vy;
                bodies.vel[i][2] = w.vz;
                bodies.vel[i][3] = kBodyRadius / s.size;
            }
            bodies.info[0] = float(count);
        }
        void* bodiesCpu = nullptr;
        const VkDeviceSize bodiesOffset = Allocate(sizeof(SmokeBodies), 256, &bodiesCpu);
        std::memcpy(bodiesCpu, &bodies, sizeof(bodies));
        VkDescriptorBufferInfo bodiesInfo{f.ring, bodiesOffset, sizeof(SmokeBodies)};
        const uint32_t c = v.cur, o = c ^ 1u;
        VkImageView lightView = v.light.view;
        // Bindings: 0 velIn, 1 densIn (sampled); 2 velOut, 3 densOut, 4 curl, 5 pIn, 6 pOut, 7 div, 8 velRead, 9 light;
        // 10 the curl image sampled (MacCormack's forward step).
        auto dispatch = [&](int stage, const SmokeImage& velIn, const SmokeImage& densIn, const SmokeImage& velOut,
                            const SmokeImage& densOut, const SmokeImage& pIn, const SmokeImage& pOut,
                            const SmokeImage& velRead, float first) {
            VkDescriptorImageInfo info[11] = {
                {m_smokeSampler, velIn.view, VK_IMAGE_LAYOUT_GENERAL},  {m_smokeSampler, densIn.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, velOut.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, densOut.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, m_smokeCurl.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pIn.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, pOut.view, VK_IMAGE_LAYOUT_GENERAL},   {VK_NULL_HANDLE, m_smokeDiv.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, velRead.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, lightView, VK_IMAGE_LAYOUT_GENERAL},
                {m_smokeSampler, m_smokeCurl.view, VK_IMAGE_LAYOUT_GENERAL},
            };
            VkWriteDescriptorSet w[12] = {};
            for (uint32_t i = 0; i < 12; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = i < 2 || i == 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[i].pImageInfo = i < 11 ? &info[i] : nullptr;
            }
            w[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[11].pBufferInfo = &bodiesInfo;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_smokeSimLayout, 0, 12, w);
            SimPush q = p;
            q.a[0] = float(stage);
            q.e[3] = first;
            vkCmdPushConstants(cmd, m_smokeSimLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
            vkCmdDispatch(cmd, groups, groups, groups);
            memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        };
        SmokeImage* pr = m_smokePressure;
        if (!m_smokeNoiseMade) {                 // (once: the wisps' noise, written through the light binding)
            lightView = m_smokeNoise.view;
            p.a[1] = float(kNoiseSize);
            groups = kNoiseSize / 4;
            dispatch(10, v.vel[c], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], 0.0f);
            lightView = v.light.view;
            p.a[1] = float(n);
            groups = n / 4;
            m_smokeNoiseMade = true;
        }
        dispatch(0, v.vel[c], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], float(m_frameNumber % 65521u));
        dispatch(1, v.vel[c], v.dens[c], v.vel[c], v.dens[o], pr[0], pr[1], v.vel[o], 0.0f);   // curl of vel[o]
        dispatch(2, v.vel[o], v.dens[c], v.vel[c], v.dens[o], pr[0], pr[1], v.vel[o], 0.0f);   // vorticity -> vel[c]
        dispatch(3, v.vel[o], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], 0.0f);   // divergence of vel[c]
        for (uint32_t k = 0; k < kJacobi; ++k)                                                  // pressure: pr[0] <-> pr[1]
            dispatch(4, v.vel[o], v.dens[c], v.vel[o], v.dens[o], pr[k & 1], pr[(k & 1) ^ 1], v.vel[c], k == 0 ? 1.0f : 0.0f);
        const SmokeImage& p0 = pr[kJacobi & 1];                                                 // (the last written)
        dispatch(5, v.vel[c], v.dens[c], v.vel[o], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // projected -> vel[o]
        dispatch(6, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // density forward -> curl
        dispatch(8, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // ... and back -> div
        dispatch(9, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // corrected -> dens[o]
        dispatch(7, v.vel[o], v.dens[o], v.vel[c], v.dens[c], p0, pr[0], v.vel[c], 0.0f);       // sunlight
        v.cur = o;
    }
    memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
           VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    ProfileMark("smoke simulation");

    // The drawing: the scene's depth readable, the colour targets without it.
    ImageBarrier(cmd, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, m_depthLayout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                 VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT,
                 VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
                 VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
    m_depthLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    EnsureRingSpace(live.size() * (sizeof(SmokeFrame) + 256) + sizeof(FrameLights) + 1024);
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    Texture* targets[5] = {m_scene, m_glow, m_localFraction, m_motionVectors, m_albedo};
    VkRenderingAttachmentInfo colors[5] = {};
    for (int i = 0; i < 5; ++i) {
        Transition(cmd, targets[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
        colors[i].imageView = targets[i]->m_view;
        colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        colors[i].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    }
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {m_scene->m_width, m_scene->m_height}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 5;
    ri.pColorAttachments = colors;
    vkCmdBeginRendering(cmd, &ri);
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};   // the scene draws into a flipped viewport
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokePipeline);
    // Far ones first (each blends over what is behind it).
    std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [dist, vp] : live) {
        SmokeVolume& v = *vp;
        SmokeFrame sf{};
        sf.viewProj = MulMatrix(m_view, m_proj);
        sf.view = m_view;
        sf.camera[0] = eye[0];
        sf.camera[1] = eye[1];
        sf.camera[2] = eye[2];
        sf.camera[3] = TaaActive() ? 1.0f : 0.0f;
        std::memcpy(sf.box, v.corner, sizeof(v.corner));
        sf.box[3] = v.source.size;
        ArgbTo4(v.source.colour, sf.colour);
        for (int i = 0; i < 3; ++i)
            sf.colour[i] = std::max(sf.colour[i], 0.1f) * P.brightness;   // (black smoke dark grey: lit and shadowed,
                                                                          //  not a flat shape)
        sf.detail[0] = P.detail;
        sf.detail[1] = 1.2f;
        sf.detail[2] = 2.5f;
        sf.detail[3] = float(v.key % 101u) * 0.137f;
        sf.colour[3] = 1.6f * P.density * std::clamp(v.age / kWarmUp, 0.0f, 1.0f);   // (fading in while it warms up)
        if (m_terrainAmbient[3] > 0.5f)
            std::memcpy(sf.ambient, m_terrainAmbient, 12);
        else
            ArgbTo4(m_rs[d3d::RS_AMBIENT], sf.ambient);
        sf.ambient[3] = float(P.quality <= 1 ? 32 : P.quality == 2 ? 48 : 64);
        sf.proj[0] = m_proj.m[2][2];
        sf.proj[1] = m_proj.m[3][2];
        sf.proj[2] = m_proj.m[0][0];
        sf.proj[3] = m_proj.m[1][1];
        sf.viewport[0] = sw;
        sf.viewport[1] = sh;
        sf.viewport[2] = 1.0f / sw;
        sf.viewport[3] = 1.0f / sh;
        ArgbTo4(m_rs[d3d::RS_FOGCOLOR], sf.fogColour);
        sf.fogColour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
        sf.fogParams[0] = AsFloatBits(m_rs[d3d::RS_FOGSTART]);
        sf.fogParams[1] = AsFloatBits(m_rs[d3d::RS_FOGEND]);
        sf.fogParams[2] = AsFloatBits(m_rs[d3d::RS_FOGDENSITY]);
        sf.fogParams[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
        void* cpu = nullptr;
        const VkDeviceSize offset = Allocate(sizeof(SmokeFrame), 256, &cpu);
        std::memcpy(cpu, &sf, sizeof(sf));
        VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(SmokeFrame)};
        VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorImageInfo densInfo{m_smokeSampler, v.dens[v.cur].view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo sunInfo{m_smokeSampler, v.light.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo depthInfo{m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo velInfo{m_smokeSampler, v.vel[v.cur].view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo noiseInfo{m_smokeRepeatSampler, m_smokeNoise.view, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w[7] = {};
        for (uint32_t i = 0; i < 7; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].descriptorType = w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &blockInfo;
        w[1].pImageInfo = &densInfo;
        w[2].pImageInfo = &sunInfo;
        w[3].pImageInfo = &depthInfo;
        w[4].pBufferInfo = &lightsInfo;
        w[5].pImageInfo = &velInfo;
        w[6].pImageInfo = &noiseInfo;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokeLayout, 0, 7, w);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    ProfileMark("smoke");
}

}  // namespace rvk
