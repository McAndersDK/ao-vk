// Enhancement: volumetric clouds (RVK_CloudsOn). The game's clouds are puffs (DisplaySystem VisualSingleCloud_t: up to
// 30 to a cloud object, each a curved textured mesh - single_cloud01..04.png - with its own place, turn, colour and
// opacity), drawn with the sky at the start of the scene. Each puff's draw is taken instead of drawn (CaptureCloudPuff:
// its world matrix, its mesh's extent, how its texture lies on it, its colour and opacity); when the sky ends - the
// first draw after it - every puff is drawn as a volume in its place (DrawClouds, cloud.vert/.frag): its texture's
// silhouette given a body - thickest where the texture is most opaque - frayed by drifting 3D noise, lit by the sun
// through itself and by the sky. The game's placement and colours stay; the clouds gain depth and light.
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kCloudVertSpirv[] = {
#include "cloud.vert.inc"
};
const uint32_t kCloudFragSpirv[] = {
#include "cloud.frag.inc"
};
const uint32_t kCloudNoiseSpirv[] = {
#include "cloud_noise.comp.inc"
};
const uint32_t kCloudHalfFragSpirv[] = {        // cloud.frag built with RVK_CLOUD_HALF
#include "cloud_half.frag.inc"
};
const uint32_t kFullscreenVertSpirv[] = {
#include "fullscreen.vert.inc"
};
const uint32_t kCloudUpFragSpirv[] = {
#include "cloud_up.frag.inc"
};
const uint32_t kCloudShadowFragSpirv[] = {
#include "cloud_shadow.frag.inc"
};

constexpr uint32_t kShadowSize = 256;           // the clouds' shadow map (pixels a side)
constexpr float kShadowArea = 3000.0f;          // ... over this much of the world round the camera
constexpr float kLayerHeight = 350.0f;          // the cloud layer above the camera (world), where the puffs' shadows
                                                // are cast from (the game's sky draws them nearer than they are)
constexpr uint32_t kShadowPuffs = 64;

struct ShadowFrame {
    float area[4], info[4], drift[4], puff[kShadowPuffs][4];
};

constexpr uint32_t kNoiseSize = 64;             // the clouds' noise image (cells a side; tiling)
constexpr uint32_t kMaxPuffs = 256;             // drawn a frame at most

// The per-puff block (cloud.vert/.frag CloudFrame, std140).
struct CloudFrame {
    d3d::Matrix viewProj, world, invWorld;
    float eye[4];          // xyz: the eye in the puff's mesh space; w: 1 = apply the TAA jitter
    float low[4], high[4]; // its box in mesh space (xyz); low.w: the thin axis (0..2); high.w: steps
    float uvU[4], uvV[4];  // the texture's coordinates over mesh space: dot(xyz, p) + w
    float colour[4];       // rgb: its colour (the game's, lit as the game lit it); a: its opacity
    float sun[4];          // xyz: towards the sun, in mesh space (unit); w: the sun's strength on it
    float sunColour[4];    // rgb; w: the density scale (RVK_CloudThick)
    float ambient[4];      // rgb: the sky's light; w: time (s)
    float noise[4];        // xyz: the noise's drift (world units); w: its scale (per world unit)
    float fogColour[4];    // rgb; w: 1 = fog on
    float fogParams[4];    // start, end, density, table mode (D3DFOG_*)
    float distance[4];     // x: the puff's middle's view depth (its fog); y: the pre-pass's clear depth; z: 1 = skip
                           // where the pre-pass's depth isn't that (half size: an opaque surface covers it later)
};

float AsFloat(uint32_t v)
{
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}

}  // namespace

void Device::SetCloudParams(const CloudParams& params) { m_cloudParams = params; }

bool Device::CreateCloudResources(std::string* error)
{
    DestroyCloudResources();
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
    si.maxLod = 16.0f;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_cloudRepeatSampler), "cloud sampler", error))
        return false;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (!Check(vkCreateSampler(m_device, &si, nullptr, &m_cloudClampSampler), "cloud sampler", error))
        return false;
    // The noise: a tiling 3D image, made once on the GPU (cloud_noise.comp).
    {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_3D;
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = {kNoiseSize, kNoiseSize, kNoiseSize};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateImage(m_allocator, &ic, &ac, &m_cloudNoise, &m_cloudNoiseAllocation, nullptr), "cloud noise",
                   error))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = m_cloudNoise;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (!Check(vkCreateImageView(m_device, &vi, nullptr, &m_cloudNoiseView), "cloud noise view", error))
            return false;
        m_cloudNoiseMade = false;
        VkDescriptorSetLayoutBinding b{0, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 1;
        sl.pBindings = &b;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_cloudNoiseSetLayout), "cloud noise set", error))
            return false;
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &m_cloudNoiseSetLayout;
        if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_cloudNoiseLayout), "cloud noise layout", error))
            return false;
        VkShaderModuleCreateInfo mi{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        mi.codeSize = sizeof(kCloudNoiseSpirv);
        mi.pCode = kCloudNoiseSpirv;
        VkShaderModule module;
        if (!Check(vkCreateShaderModule(m_device, &mi, nullptr, &module), "cloud noise module", error))
            return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, module,
                    "main", nullptr};
        ci.layout = m_cloudNoiseLayout;
        const bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_cloudNoisePipeline),
                              "cloud noise pipeline", error);
        vkDestroyShaderModule(m_device, module, nullptr);
        if (!ok)
            return false;
    }
    // The drawing: 0 the block, 1 the puff's texture, 2 the noise, 3 the pre-pass's depth (half size), 4 the frame lights.
    VkDescriptorSetLayoutBinding b[5] = {
        {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
        {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 5;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_cloudSetLayout), "cloud set", error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_cloudSetLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_cloudLayout), "cloud layout", error))
        return false;
    // The shadow map: 0 its block, 1 the noise.
    {
        VkDescriptorSetLayoutBinding sb[2] = {
            {0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};
        VkDescriptorSetLayoutCreateInfo sbl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sbl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sbl.bindingCount = 2;
        sbl.pBindings = sb;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &sbl, nullptr, &m_cloudShadowSetLayout), "cloud shadow set", error))
            return false;
        VkPipelineLayoutCreateInfo spl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        spl.setLayoutCount = 1;
        spl.pSetLayouts = &m_cloudShadowSetLayout;
        if (!Check(vkCreatePipelineLayout(m_device, &spl, nullptr, &m_cloudShadowLayout), "cloud shadow layout", error))
            return false;
    }
    // The half size's upsampling: 0 the clouds (linear); push: the scene's size.
    {
        VkDescriptorSetLayoutBinding u{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ul{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ul.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ul.bindingCount = 1;
        ul.pBindings = &u;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &ul, nullptr, &m_cloudUpSetLayout), "cloud up set", error))
            return false;
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};
        VkPipelineLayoutCreateInfo upl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        upl.setLayoutCount = 1;
        upl.pSetLayouts = &m_cloudUpSetLayout;
        upl.pushConstantRangeCount = 1;
        upl.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(m_device, &upl, nullptr, &m_cloudUpLayout), "cloud up layout", error))
            return false;
    }
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "cloud module", error);
    };
    // Premultiplied over its targets; in the scene's rendering (its depth format) or its own (none).
    auto pipeline = [&](const uint32_t* vertCode, size_t vertSize, const uint32_t* fragCode, size_t fragSize,
                        VkPipelineLayout layout, const VkFormat* formats, uint32_t count, VkFormat depth, VkPipeline* out) {
        VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
        bool ok = module(vertCode, vertSize, &vert) && module(fragCode, fragSize, &frag);
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
            rs.cullMode = VK_CULL_MODE_NONE;     // (cloud.frag keeps the far faces)
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            // (No depth test: the sky is behind everything drawn after it.)
            VkPipelineColorBlendAttachmentState att[5] = {};
            for (auto& a : att) {
                a.colorWriteMask = 0xF;
                a.blendEnable = VK_TRUE;
                a.srcColorBlendFactor = a.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
                a.dstColorBlendFactor = a.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
                a.colorBlendOp = a.alphaBlendOp = VK_BLEND_OP_ADD;
            }
            VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            cb.attachmentCount = count;
            cb.pAttachments = att;
            VkPipelineRenderingCreateInfo rendering{VK_STRUCTURE_TYPE_PIPELINE_RENDERING_CREATE_INFO};
            rendering.colorAttachmentCount = count;
            rendering.pColorAttachmentFormats = formats;
            rendering.depthAttachmentFormat = depth;
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
            ci.layout = layout;
            ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, out),
                       "vkCreateGraphicsPipelines (clouds)", error);
        }
        if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
        return ok;
    };
    const VkFormat sceneFormats[5] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA16F).vk,
                                      GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RGBA16F).vk,
                                      GetFormatInfo(Format::A8R8G8B8).vk};
    const VkFormat halfFormat = GetFormatInfo(Format::RGBA16F).vk, shadowFormat = GetFormatInfo(Format::RGBA8).vk;
    return pipeline(kCloudVertSpirv, sizeof(kCloudVertSpirv), kCloudFragSpirv, sizeof(kCloudFragSpirv), m_cloudLayout,
                    sceneFormats, 5, kDepthFormat, &m_cloudPipeline) &&
           pipeline(kCloudVertSpirv, sizeof(kCloudVertSpirv), kCloudHalfFragSpirv, sizeof(kCloudHalfFragSpirv),
                    m_cloudLayout, &halfFormat, 1, VK_FORMAT_UNDEFINED, &m_cloudHalfPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudUpFragSpirv, sizeof(kCloudUpFragSpirv),
                    m_cloudUpLayout, sceneFormats, 5, VK_FORMAT_UNDEFINED, &m_cloudUpPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudUpFragSpirv, sizeof(kCloudUpFragSpirv),
                    m_cloudUpLayout, sceneFormats, 5, kDepthFormat, &m_cloudUpScenePipeline) &&
           // (Blended over with alpha 1: written.)
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudShadowFragSpirv, sizeof(kCloudShadowFragSpirv),
                    m_cloudShadowLayout, &shadowFormat, 1, VK_FORMAT_UNDEFINED, &m_cloudShadowPipeline);
}

void Device::DestroyCloudResources()
{
    for (VkPipeline* p : {&m_cloudPipeline, &m_cloudHalfPipeline, &m_cloudUpPipeline, &m_cloudUpScenePipeline,
                          &m_cloudShadowPipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_cloudShadowLayout) { vkDestroyPipelineLayout(m_device, m_cloudShadowLayout, nullptr); m_cloudShadowLayout = VK_NULL_HANDLE; }
    if (m_cloudShadowSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_cloudShadowSetLayout, nullptr); m_cloudShadowSetLayout = VK_NULL_HANDLE; }
    if (m_cloudShadowMap) { DestroyTexture(m_cloudShadowMap); m_cloudShadowMap = nullptr; }
    if (m_cloudUpLayout) { vkDestroyPipelineLayout(m_device, m_cloudUpLayout, nullptr); m_cloudUpLayout = VK_NULL_HANDLE; }
    if (m_cloudUpSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_cloudUpSetLayout, nullptr); m_cloudUpSetLayout = VK_NULL_HANDLE; }
    if (m_cloudHalf) { DestroyTexture(m_cloudHalf); m_cloudHalf = nullptr; }
    if (m_cloudNoisePipeline) { vkDestroyPipeline(m_device, m_cloudNoisePipeline, nullptr); m_cloudNoisePipeline = VK_NULL_HANDLE; }
    if (m_cloudLayout) { vkDestroyPipelineLayout(m_device, m_cloudLayout, nullptr); m_cloudLayout = VK_NULL_HANDLE; }
    if (m_cloudNoiseLayout) { vkDestroyPipelineLayout(m_device, m_cloudNoiseLayout, nullptr); m_cloudNoiseLayout = VK_NULL_HANDLE; }
    if (m_cloudSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_cloudSetLayout, nullptr); m_cloudSetLayout = VK_NULL_HANDLE; }
    if (m_cloudNoiseSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_cloudNoiseSetLayout, nullptr); m_cloudNoiseSetLayout = VK_NULL_HANDLE; }
    if (m_cloudNoiseView) { vkDestroyImageView(m_device, m_cloudNoiseView, nullptr); m_cloudNoiseView = VK_NULL_HANDLE; }
    if (m_cloudNoise) { vmaDestroyImage(m_allocator, m_cloudNoise, m_cloudNoiseAllocation); m_cloudNoise = VK_NULL_HANDLE; }
    for (VkSampler* s : {&m_cloudRepeatSampler, &m_cloudClampSampler})
        if (*s) { vkDestroySampler(m_device, *s, nullptr); *s = VK_NULL_HANDLE; }
    m_cloudPuffs.clear();
}

// A draw of the game's cloud puff (VisualSingleCloud_t, in the sky): taken, not drawn. Its mesh is a curved sheet with
// the puff's texture on it; what is kept is its extent, how the texture lies over it (u, v fitted linearly over the
// mesh's positions), its colour as the game would light it, and its opacity.
bool Device::CaptureCloudPuff(uint32_t fvf, const void* vertices, uint32_t vertexCount, const uint16_t* indices,
                              uint32_t indexCount)
{
    if (!m_cloudParams.enable || !m_cloudPipeline || m_external || !m_scenePhase || m_target != m_scene || !vertices ||
        vertexCount < 3 || (fvf & d3d::FVF_POSITION_MASK) != d3d::FVF_XYZ)
        return false;
    if (m_cloudPuffs.size() >= kMaxPuffs)
        return true;                              // (hidden all the same)
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    CloudPuff p{};
    p.world = m_world;
    p.view = m_view;
    p.proj = m_proj;
    p.texture = m_textures[0];
    for (int k = 0; k < 3; ++k) { p.low[k] = 1e30f; p.high[k] = -1e30f; }
    // Least squares: u, v = a . (x, y, z, 1) - the normal equations' sums.
    double ata[4][4] = {}, atu[4] = {}, atv[4] = {};
    double colour[4] = {};
    const bool hasUv = layout.offset[4] >= 0, hasColour = layout.offset[2] >= 0;
    for (uint32_t i = 0; i < vertexCount; ++i) {
        const uint8_t* v = base + size_t(i) * layout.stride;
        float pos[3];
        std::memcpy(pos, v + layout.offset[0], sizeof(pos));
        for (int k = 0; k < 3; ++k) {
            p.low[k] = std::min(p.low[k], pos[k]);
            p.high[k] = std::max(p.high[k], pos[k]);
        }
        if (hasUv) {
            float uv[2];
            std::memcpy(uv, v + layout.offset[4], sizeof(uv));
            const double a[4] = {pos[0], pos[1], pos[2], 1.0};
            for (int r = 0; r < 4; ++r) {
                for (int c = 0; c < 4; ++c) ata[r][c] += a[r] * a[c];
                atu[r] += a[r] * uv[0];
                atv[r] += a[r] * uv[1];
            }
        }
        if (hasColour) {
            uint32_t argb;
            std::memcpy(&argb, v + layout.offset[2], 4);
            colour[0] += ((argb >> 16) & 0xFF) / 255.0;
            colour[1] += ((argb >> 8) & 0xFF) / 255.0;
            colour[2] += (argb & 0xFF) / 255.0;
            colour[3] += (argb >> 24) / 255.0;
        }
    }
    // The thin axis: the mesh's least extent (a sheet); the texture's fit (Gauss-Jordan; a flat sheet makes the
    // thin axis's column degenerate - regularised).
    p.thin = 0;
    for (int k = 1; k < 3; ++k)
        if (p.high[k] - p.low[k] < p.high[p.thin] - p.low[p.thin]) p.thin = k;
    if (hasUv) {
        double m[4][6];
        for (int r = 0; r < 4; ++r) {
            for (int c = 0; c < 4; ++c) m[r][c] = ata[r][c] + (r == c ? 1e-6 * (ata[r][r] + 1.0) : 0.0);
            m[r][4] = atu[r];
            m[r][5] = atv[r];
        }
        for (int c = 0; c < 4; ++c) {
            int pivot = c;
            for (int r = c + 1; r < 4; ++r)
                if (std::fabs(m[r][c]) > std::fabs(m[pivot][c])) pivot = r;
            for (int k = 0; k < 6; ++k) std::swap(m[c][k], m[pivot][k]);
            const double d = m[c][c];
            if (std::fabs(d) < 1e-12)
                continue;
            for (int k = 0; k < 6; ++k) m[c][k] /= d;
            for (int r = 0; r < 4; ++r) {
                if (r == c) continue;
                const double f = m[r][c];
                for (int k = 0; k < 6; ++k) m[r][k] -= f * m[c][k];
            }
        }
        for (int r = 0; r < 4; ++r) {
            p.uvU[r] = float(m[r][4]);
            p.uvV[r] = float(m[r][5]);
        }
    } else {
        // (No texture coordinates: the sheet's extent as 0..1.)
        const int a = p.thin == 0 ? 1 : 0, b = p.thin == 2 ? 1 : 2;
        p.uvU[a] = 1.0f / std::max(p.high[a] - p.low[a], 1e-4f);
        p.uvU[3] = -p.low[a] * p.uvU[a];
        p.uvV[b] = 1.0f / std::max(p.high[b] - p.low[b], 1e-4f);
        p.uvV[3] = -p.low[b] * p.uvV[b];
    }
    // Its colour as the game lit it (about: the vertex colour times its emissive, the ambient and some sun) and its
    // opacity (the vertex alpha - the game fades puffs in and out through it - times the material's).
    float c[4] = {1.0f, 1.0f, 1.0f, 1.0f};
    if (hasColour)
        for (int k = 0; k < 4; ++k) c[k] = float(colour[k] / vertexCount);
    else
        c[0] = m_material.diffuse.r, c[1] = m_material.diffuse.g, c[2] = m_material.diffuse.b, c[3] = m_material.diffuse.a;
    const uint32_t amb = m_rs[d3d::RS_AMBIENT];
    const float ambient[3] = {((amb >> 16) & 0xFF) / 255.0f, ((amb >> 8) & 0xFF) / 255.0f, (amb & 0xFF) / 255.0f};
    const float emissive[3] = {m_material.emissive.r, m_material.emissive.g, m_material.emissive.b};
    for (int k = 0; k < 3; ++k)
        p.colour[k] = c[k] * std::min(emissive[k] + ambient[k] + 0.3f, 1.0f);   // (the game's light saturates at 1)
    p.colour[3] = c[3];
    // The fog it is drawn with (the game's sky may draw its clouds with or without).
    p.fogColour[0] = ((m_rs[d3d::RS_FOGCOLOR] >> 16) & 0xFF) / 255.0f;
    p.fogColour[1] = ((m_rs[d3d::RS_FOGCOLOR] >> 8) & 0xFF) / 255.0f;
    p.fogColour[2] = (m_rs[d3d::RS_FOGCOLOR] & 0xFF) / 255.0f;
    p.fogColour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
    p.fogParams[0] = AsFloat(m_rs[d3d::RS_FOGSTART]);
    p.fogParams[1] = AsFloat(m_rs[d3d::RS_FOGEND]);
    p.fogParams[2] = AsFloat(m_rs[d3d::RS_FOGDENSITY]);
    p.fogParams[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
    // (What the game's puffs are like: a few logged, now and then.)
    static uint32_t logged = 0;
    static uint64_t logFrame = 0;
    if (m_frameNumber >= logFrame && logged < 60) {
        const float* w = &p.world.m[0][0];
        Log("clouds: puff %zu: %u vertices, %u indices, extent %.2f..%.2f %.2f..%.2f %.2f..%.2f (thin %d), uv u %.3f %.3f "
            "%.3f %+.3f v %.3f %.3f %.3f %+.3f, colour %.2f %.2f %.2f a %.2f, world scale %.1f %.1f %.1f at %.1f %.1f %.1f, "
            "texture %ux%u, fog %s %.0f..%.0f",
            m_cloudPuffs.size(), vertexCount, indexCount, p.low[0], p.high[0], p.low[1], p.high[1], p.low[2], p.high[2],
            p.thin, p.uvU[0], p.uvU[1], p.uvU[2], p.uvU[3], p.uvV[0], p.uvV[1], p.uvV[2], p.uvV[3], p.colour[0],
            p.colour[1], p.colour[2], p.colour[3], std::sqrt(w[0] * w[0] + w[1] * w[1] + w[2] * w[2]),
            std::sqrt(w[4] * w[4] + w[5] * w[5] + w[6] * w[6]), std::sqrt(w[8] * w[8] + w[9] * w[9] + w[10] * w[10]),
            w[12], w[13], w[14], p.texture ? p.texture->m_width : 0u, p.texture ? p.texture->m_height : 0u,
            p.fogColour[3] > 0.5f ? "on" : "off", p.fogParams[0], p.fogParams[1]);
        if (++logged % 6 == 0)
            logFrame = m_frameNumber + 600;
    }
    m_cloudPuffs.push_back(p);
    (void)indices;
    (void)indexCount;
    return true;
}

// The puffs taken this frame, drawn as volumes where the sky ends (no depth test: only the sky is there yet). Far ones
// first.
//
// RVK_CloudHalf marches them into a half-size target and filters that up over the scene's targets; RVK_CloudShadow
// makes their shadow map first. Both need their own renderings - outside the scene's. With the depth pre-pass armed
// (prepass.cpp: the scene's rendering split at its depth clear) they are recorded into their own commands, run after
// the pre-pass and before the scene's draws (Frame::clouds): the scene's rendering - and its pre-pass segment - go on
// unbroken, and the march skips every pixel the pre-pass found an opaque surface at (the scene's draws cover it
// later). Without the pre-pass, the scene's rendering is ended for them and begun again (and nothing is skipped).
void Device::DrawClouds(VkCommandBuffer cmd)
{
    m_cloudsDrawn = true;
    if (m_cloudPuffs.empty() || !m_cloudPipeline || !m_rendering)
        return;
    FlushGroup();
    ProfileSceneClass(0);
    EnsureRingSpace(m_cloudPuffs.size() * (sizeof(CloudFrame) + 256) + sizeof(FrameLights) + sizeof(ShadowFrame) + 2048);
    cmd = m_frames[m_frameIndex].main;           // (a flush there submits and swaps the frame's commands)
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    Frame& f = m_frames[m_frameIndex];
    // Each puff's distance (its box's middle in view space).
    std::vector<std::pair<float, const CloudPuff*>> order;
    order.reserve(m_cloudPuffs.size());
    for (const CloudPuff& p : m_cloudPuffs) {
        float mid[3], w[3], z = 0.0f;
        for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (p.low[k] + p.high[k]);
        for (int j = 0; j < 3; ++j)
            w[j] = mid[0] * p.world.m[0][j] + mid[1] * p.world.m[1][j] + mid[2] * p.world.m[2][j] + p.world.m[3][j];
        z = w[0] * p.view.m[0][2] + w[1] * p.view.m[1][2] + w[2] * p.view.m[2][2] + p.view.m[3][2];
        order.emplace_back(z, &p);   // (the view depth: the fog's distance too, as the game's per-vertex fog about)
    }
    std::sort(order.begin(), order.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    const uint32_t fw = m_scene->m_width, fh = m_scene->m_height, hw = (fw + 1) / 2, hh = (fh + 1) / 2;
    if (m_cloudParams.half && (!m_cloudHalf || m_cloudHalf->m_width != hw || m_cloudHalf->m_height != hh)) {
        if (m_cloudHalf) DestroyTexture(m_cloudHalf);
        m_cloudHalf = CreateImage(hw, hh, Format::RGBA16F, 1, true);
    }
    const bool half = m_cloudParams.half && m_cloudHalf && m_cloudHalfPipeline && m_cloudUpPipeline &&
                      m_cloudUpScenePipeline;
    const bool shadow = m_cloudParams.shadow > 0.0f && m_cloudShadowPipeline;
    const bool own = (half || shadow) && f.split && m_prepassArmed && f.clouds && !f.cloudsRecorded;
    const bool skipCovered = own && half && m_prepassFullClear;   // (else what the pre-pass didn't clear isn't sky)
    ProfileFrame& prof = m_profile[m_frameIndex];
    VkCommandBuffer work = cmd;                  // the shadow map's and the half-size march's commands
    if (own) {
        VkCommandBufferBeginInfo b{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        vkBeginCommandBuffer(f.clouds, &b);
        work = f.clouds;
        if (prof.pool)
            vkCmdWriteTimestamp2(work, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof.pool, kProfileMarks);
    } else if (half || shadow) {
        m_renderEndCause = kEndOther;
        EndRendering();
    }
    auto endOwn = [&] {
        if (prof.pool) {
            vkCmdWriteTimestamp2(work, VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT, prof.pool, kProfileMarks + 1);
            prof.cloudsTimed = true;
        }
        vkEndCommandBuffer(work);
        f.cloudsRecorded = true;
    };
    if (shadow) {
        MakeCloudShadow(work);
        m_frameLightsDirty = true;                // (the scene's draws after the sky read it through the frame lights)
    }
    if (!half) {
        if (own)
            endOwn();
        else if (shadow)
            BeginRenderingOn(m_target);           // the puffs into the scene's rendering
        float lo[2], hi[2];
        MarchCloudPuffs(cmd, order, false, false, lo, hi);
    } else {
        const VkPipelineStageFlags2 tests =
            VK_PIPELINE_STAGE_2_EARLY_FRAGMENT_TESTS_BIT | VK_PIPELINE_STAGE_2_LATE_FRAGMENT_TESTS_BIT;
        if (skipCovered)                          // the pre-pass's depth, read (its layout put back after)
            ImageBarrier(work, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, tests, VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT,
                         VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
        Transition(work, m_cloudHalf, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = m_cloudHalf->m_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {hw, hh}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(work, &ri);
        float lo[2], hi[2];
        MarchCloudPuffs(work, order, true, skipCovered, lo, hi);
        vkCmdEndRendering(work);
        Transition(work, m_cloudHalf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        if (skipCovered)
            ImageBarrier(work, m_depth, VK_IMAGE_ASPECT_DEPTH_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_IMAGE_LAYOUT_DEPTH_ATTACHMENT_OPTIMAL, VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT, 0, tests,
                         VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_READ_BIT | VK_ACCESS_2_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT);
        if (own)
            endOwn();
        // Filtered up over the scene's targets where the puffs are: inside the scene's rendering (own commands), or
        // in one of its own and the scene's begun again. (The box's thickness was widened in the draw; a margin covers
        // it and the filter.)
        const float margin = 0.25f * std::max(hi[0] - lo[0], hi[1] - lo[1]) + 4.0f;
        const int x0 = std::max(int(lo[0] - margin), 0), y0 = std::max(int(lo[1] - margin), 0);
        const int x1 = std::min(int(hi[0] + margin), int(fw)), y1 = std::min(int(hi[1] + margin), int(fh));
        if (x1 > x0 && y1 > y0) {
            if (!own) {
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
                VkRenderingInfo sri{VK_STRUCTURE_TYPE_RENDERING_INFO};
                sri.renderArea = {{0, 0}, {fw, fh}};
                sri.layerCount = 1;
                sri.colorAttachmentCount = 5;
                sri.pColorAttachments = colors;
                vkCmdBeginRendering(cmd, &sri);
            }
            VkViewport full{0.0f, 0.0f, float(fw), float(fh), 0.0f, 1.0f};
            VkRect2D area{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            vkCmdSetViewport(cmd, 0, 1, &full);
            vkCmdSetScissor(cmd, 0, 1, &area);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, own ? m_cloudUpScenePipeline : m_cloudUpPipeline);
            VkDescriptorImageInfo image{m_cloudClampSampler, m_cloudHalf->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
            w.dstBinding = 0;
            w.descriptorCount = 1;
            w.descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w.pImageInfo = &image;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudUpLayout, 0, 1, &w);
            const float size[4] = {float(fw), float(fh), 0.0f, 0.0f};
            vkCmdPushConstants(cmd, m_cloudUpLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(size), size);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            if (!own)
                vkCmdEndRendering(cmd);
        }
        if (!own)
            BeginRenderingOn(m_target);          // the scene's rendering again, for its draws after the sky
    }
    m_cache = StateCache{};                      // this pipeline is not the scene's
    m_arenaBound = m_bindlessBound = false;      // ... and its pushed set 0 replaced the scene's
    ProfileMark("clouds");
}

// The puffs (far ones first) marched: into the scene's rendering (full size) or the half-size target's (half), its
// viewport and pipeline set here. skipCovered: not where the pre-pass's depth (readable) found an opaque surface.
// lo / hi: where the puffs are on the scene's screen (pixels).
void Device::MarchCloudPuffs(VkCommandBuffer cmd, const std::vector<std::pair<float, const CloudPuff*>>& order,
                             bool half, bool skipCovered, float lo[2], float hi[2])
{
    Frame& f = m_frames[m_frameIndex];
    const uint32_t fw = m_scene->m_width, fh = m_scene->m_height, hw = (fw + 1) / 2, hh = (fh + 1) / 2;
    const float sw = float(fw), sh = float(fh), k = half ? 0.5f : 1.0f;
    VkViewport viewport{0.5f * k, (sh + 0.5f) * k, sw * k, -sh * k, 0.0f, 1.0f};   // the scene's flipped viewport (halved)
    VkRect2D scissor{{0, 0}, {half ? hw : fw, half ? hh : fh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, half ? m_cloudHalfPipeline : m_cloudPipeline);
    lo[0] = sw, lo[1] = sh, hi[0] = hi[1] = 0.0f;
    float* screenLo = lo;
    float* screenHi = hi;
    // Towards the sun (the frame's), its colour.
    float sun[3] = {-m_frameSunDir[0], -m_frameSunDir[1], -m_frameSunDir[2]};
    const float sl = std::max(std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]), 1e-6f);
    for (float& s : sun) s /= sl;
    const double t = std::fmod(m_frameClock, 3600.0);
    for (const auto& [z, pp] : order) {
        const CloudPuff& p = *pp;
        d3d::Matrix inv;
        if (!p.texture || !InvertMatrix(p.world, &inv))
            continue;                            // (its silhouette is its texture)
        CloudFrame cf{};
        cf.viewProj = MulMatrix(p.view, p.proj);
        {
            const d3d::Matrix wvp = MulMatrix(p.world, cf.viewProj);
            for (int c = 0; c < 8; ++c) {
                const float q[3] = {c & 1 ? p.high[0] : p.low[0], c & 2 ? p.high[1] : p.low[1], c & 4 ? p.high[2] : p.low[2]};
                float clip[4];
                for (int j = 0; j < 4; ++j)
                    clip[j] = q[0] * wvp.m[0][j] + q[1] * wvp.m[1][j] + q[2] * wvp.m[2][j] + wvp.m[3][j];
                if (clip[3] < 1e-3f) {           // (round the camera: anywhere)
                    screenLo[0] = screenLo[1] = 0.0f;
                    screenHi[0] = sw;
                    screenHi[1] = sh;
                    continue;
                }
                const float x = 0.5f + (clip[0] / clip[3] + 1.0f) * 0.5f * sw;
                const float y = sh + 0.5f - (clip[1] / clip[3] + 1.0f) * 0.5f * sh;
                screenLo[0] = std::min(screenLo[0], x);
                screenLo[1] = std::min(screenLo[1], y);
                screenHi[0] = std::max(screenHi[0], x);
                screenHi[1] = std::max(screenHi[1], y);
            }
        }
        cf.world = p.world;
        cf.invWorld = inv;
        // The eye: the view's inverse translation, into mesh space.
        const auto& vm = p.view.m;
        float eyeW[3];
        for (int i = 0; i < 3; ++i)
            eyeW[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
        for (int j = 0; j < 3; ++j)
            cf.eye[j] = eyeW[0] * inv.m[0][j] + eyeW[1] * inv.m[1][j] + eyeW[2] * inv.m[2][j] + inv.m[3][j];
        cf.eye[3] = TaaActive() ? 1.0f : 0.0f;
        // The box: the sheet given a body across its thin axis, half as thick as it is wide.
        float low[3], high[3];
        std::memcpy(low, p.low, sizeof(low));
        std::memcpy(high, p.high, sizeof(high));
        const int a = p.thin == 0 ? 1 : 0, b = p.thin == 2 ? 1 : 2;
        const float width = std::min(high[a] - low[a], high[b] - low[b]);
        const float mid = 0.5f * (low[p.thin] + high[p.thin]);
        const float thick = std::max(0.5f * (high[p.thin] - low[p.thin]), 0.25f * width);
        low[p.thin] = mid - thick;
        high[p.thin] = mid + thick;
        std::memcpy(cf.low, low, sizeof(low));
        std::memcpy(cf.high, high, sizeof(high));
        cf.low[3] = float(p.thin);
        cf.high[3] = float(std::clamp(m_cloudParams.steps, 8u, 96u));
        std::memcpy(cf.uvU, p.uvU, sizeof(cf.uvU));
        std::memcpy(cf.uvV, p.uvV, sizeof(cf.uvV));
        // Rain (rvk rain.cpp: how hard, eased): the clouds heavier - darker, greyer, denser.
        const float storm = std::clamp(m_rain, 0.0f, 1.0f);
        const float grey = (p.colour[0] + p.colour[1] + p.colour[2]) / 3.0f;
        for (int k = 0; k < 3; ++k)
            cf.colour[k] = (p.colour[k] + (grey - p.colour[k]) * 0.6f * storm) * (1.0f - 0.5f * storm) * m_cloudParams.brightness;
        cf.colour[3] = std::clamp(p.colour[3], 0.0f, 1.0f);
        // The sun into mesh space (a direction: the inverse's 3x3; normalised in the shader).
        for (int j = 0; j < 3; ++j)
            cf.sun[j] = sun[0] * inv.m[0][j] + sun[1] * inv.m[1][j] + sun[2] * inv.m[2][j];
        cf.sun[3] = 1.0f;
        cf.sunColour[0] = m_frameSunColor[0];
        cf.sunColour[1] = m_frameSunColor[1];
        cf.sunColour[2] = m_frameSunColor[2];
        cf.sunColour[3] = m_cloudParams.density * (1.0f + storm);
        if (m_terrainAmbient[3] > 0.5f)
            std::memcpy(cf.ambient, m_terrainAmbient, 12);
        else
            cf.ambient[0] = cf.ambient[1] = cf.ambient[2] = 0.35f;
        cf.ambient[3] = float(t);
        cf.noise[0] = float(std::fmod(t * 1.5, 4096.0));   // (drifting with the wind; evolving through y)
        cf.noise[1] = float(std::fmod(t * 0.35, 4096.0));
        cf.noise[2] = float(std::fmod(t * 0.6, 4096.0));
        cf.noise[3] = m_cloudParams.detail;
        std::memcpy(cf.fogColour, p.fogColour, sizeof(cf.fogColour));
        std::memcpy(cf.fogParams, p.fogParams, sizeof(cf.fogParams));
        cf.distance[0] = z;
        cf.distance[1] = m_prepassClearZ;
        cf.distance[2] = skipCovered ? 1.0f : 0.0f;
        void* cpu = nullptr;
        const VkDeviceSize offset = Allocate(sizeof(CloudFrame), 256, &cpu);
        std::memcpy(cpu, &cf, sizeof(cf));
        VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(CloudFrame)};
        VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorImageInfo texInfo{m_cloudClampSampler, p.texture->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo noiseInfo{m_cloudRepeatSampler, m_cloudNoiseView, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo depthInfo{m_pointSampler, skipCovered ? m_depthView : m_blackTexture->m_view,
                                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkWriteDescriptorSet w[5] = {};
        for (uint32_t i = 0; i < 5; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].descriptorCount = 1;
        }
        w[0].dstBinding = 0;
        w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &blockInfo;
        w[1].dstBinding = 1;
        w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[1].pImageInfo = &texInfo;
        w[2].dstBinding = 2;
        w[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[2].pImageInfo = &noiseInfo;
        w[3].dstBinding = 4;
        w[3].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[3].pBufferInfo = &lightsInfo;
        w[4].dstBinding = 3;                     // (half size: the pre-pass's depth)
        w[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        w[4].pImageInfo = &depthInfo;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudLayout, 0, half ? 5 : 4, w);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
}

// The clouds' shadow map: each puff where it would be in the world - out along the camera's line to it, as far as the
// cloud layer (kLayerHeight above the camera; the game's sky draws them nearer, scaled down) - and as much bigger; its
// footprint there, as opaque as the puff. Outside any rendering.
void Device::MakeCloudShadow(VkCommandBuffer cmd)
{
    if (!m_cloudShadowMap) {
        m_cloudShadowMap = CreateImage(kShadowSize, kShadowSize, Format::RGBA8, 1, true);
        if (!m_cloudShadowMap)
            return;
    }
    const auto& vm = m_cloudPuffs.front().view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    static const float layerHeight = getenv("RANDYVK_CLOUD_LAYER") ? float(atof(getenv("RANDYVK_CLOUD_LAYER"))) : kLayerHeight;
    const float layer = eye[1] + layerHeight;   // (RANDYVK_CLOUD_LAYER: tests with a small sky; the map shrinks alike)
    const float area = kShadowArea * layerHeight / kLayerHeight;
    ShadowFrame sf{};
    const float texel = area / float(kShadowSize);
    // (The map's corner on whole texels: it doesn't shimmer as the camera moves.)
    sf.area[0] = std::floor((eye[0] - area * 0.5f) / texel) * texel;
    sf.area[1] = std::floor((eye[2] - area * 0.5f) / texel) * texel;
    sf.area[2] = area;
    sf.info[0] = float(std::fmod(m_frameClock, 3600.0));
    sf.info[1] = float(kShadowSize);
    // How cloudy the sky is: the puffs' opacity times how much of the view round they take (their angular size), plus
    // rain - for the drifting field of shadows.
    float cover = 0.0f;
    for (const CloudPuff& p : m_cloudPuffs) {
        float mid[3], w[3];
        for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (p.low[k] + p.high[k]);
        for (int j = 0; j < 3; ++j)
            w[j] = mid[0] * p.world.m[0][j] + mid[1] * p.world.m[1][j] + mid[2] * p.world.m[2][j] + p.world.m[3][j];
        const float len = std::max(std::sqrt((w[0] - eye[0]) * (w[0] - eye[0]) + (w[1] - eye[1]) * (w[1] - eye[1]) +
                                             (w[2] - eye[2]) * (w[2] - eye[2])), 1.0f);
        float extent = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (k == p.thin) continue;
            const float* row = p.world.m[k];
            extent = std::max(extent, (p.high[k] - p.low[k]) * std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
        }
        cover += std::clamp(p.colour[3], 0.0f, 1.0f) * (extent / len) * (extent / len) * 0.5f;
    }
    sf.info[2] = std::clamp(cover * 0.6f + std::clamp(m_rain, 0.0f, 1.0f) * 0.5f, 0.0f, 1.0f) *
                 std::min(m_cloudParams.density, 1.5f);
    sf.info[2] = std::min(sf.info[2], 1.0f);
    sf.info[3] = area * 0.2f;                    // (a few cloud shadows across the map)
    // Drifting (the wind's way when there is one - the rain's - else a slow breeze), 6 world units a second.
    {
        float wx = m_rainWind[0], wz = m_rainWind[2];
        const float wl = std::sqrt(wx * wx + wz * wz);
        if (wl < 0.1f) { wx = 0.8f; wz = 0.6f; } else { wx /= wl; wz /= wl; }
        const float t = float(std::fmod(m_frameClock, 3600.0));
        sf.drift[0] = -wx * t * 6.0f;
        sf.drift[1] = -wz * t * 6.0f;
    }
    uint32_t count = 0;
    for (const CloudPuff& p : m_cloudPuffs) {
        if (count >= kShadowPuffs || !p.texture)
            continue;
        float mid[3], w[3];
        for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (p.low[k] + p.high[k]);
        for (int j = 0; j < 3; ++j)
            w[j] = mid[0] * p.world.m[0][j] + mid[1] * p.world.m[1][j] + mid[2] * p.world.m[2][j] + p.world.m[3][j];
        const float d[3] = {w[0] - eye[0], w[1] - eye[1], w[2] - eye[2]};
        const float len = std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]);
        if (len < 1e-3f || d[1] < 0.08f * len)
            continue;                            // (on the horizon: no shadow near)
        const float t = (layer - eye[1]) / d[1];
        // Its width in the world as drawn: the sheet's wider extent through the world matrix.
        float extent = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (k == p.thin) continue;
            const float* row = p.world.m[k];
            extent = std::max(extent, (p.high[k] - p.low[k]) * std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
        }
        sf.puff[count][0] = eye[0] + d[0] * t;
        sf.puff[count][1] = eye[2] + d[2] * t;
        sf.puff[count][2] = std::max(0.45f * extent * t, 1.0f);
        sf.puff[count][3] = std::clamp(p.colour[3] * std::min(m_cloudParams.density, 1.5f) * 0.85f, 0.0f, 1.0f);
        ++count;
    }
    sf.area[3] = float(count);
    if (m_frameNumber % 600 == 1 || m_frameNumber == 30)
        Log("clouds: shadow map: %u puffs overhead (of %zu), cloudy %.2f, layer at %.0f, area from %.0f %.0f; the first at "
            "%.0f %.0f r %.0f opacity %.2f", count, m_cloudPuffs.size(), sf.info[2], layer, sf.area[0], sf.area[1],
            count ? sf.puff[0][0] : 0.0f,
            count ? sf.puff[0][1] : 0.0f, count ? sf.puff[0][2] : 0.0f, count ? sf.puff[0][3] : 0.0f);
    m_cloudShadowArea[0] = sf.area[0];
    m_cloudShadowArea[1] = sf.area[1];
    m_cloudShadowArea[2] = area;
    m_cloudShadowArea[3] = layer;
    m_cloudShadowFrame = m_frameNumber;
    EnsureRingSpace(sizeof(ShadowFrame) + 512);
    Frame& f = m_frames[m_frameIndex];
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(ShadowFrame), 256, &cpu);
    std::memcpy(cpu, &sf, sizeof(sf));
    Transition(cmd, m_cloudShadowMap, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_cloudShadowMap->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kShadowSize, kShadowSize}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(kShadowSize), float(kShadowSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kShadowSize, kShadowSize}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowPipeline);
    VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(ShadowFrame)};
    VkDescriptorImageInfo noiseInfo{m_cloudRepeatSampler, m_cloudNoiseView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w[2] = {};
    for (uint32_t i = 0; i < 2; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
    }
    w[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    w[0].pBufferInfo = &blockInfo;
    w[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    w[1].pImageInfo = &noiseInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudShadowLayout, 0, 2, w);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudShadowMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

VkImageView Device::CloudShadowView() const
{
    return m_cloudShadowMap && m_cloudShadowMap->m_layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
               ? m_cloudShadowMap->m_view
               : m_blackTexture->m_view;
}

// Once (the first frame): the clouds' noise, before the scene's rendering opens.
void Device::MakeCloudNoise(VkCommandBuffer cmd)
{
    if (m_cloudNoiseMade || !m_cloudNoisePipeline || !m_cloudParams.enable)
        return;
    m_cloudNoiseMade = true;
    VkImageMemoryBarrier2 b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2};
    b.srcStageMask = VK_PIPELINE_STAGE_2_TOP_OF_PIPE_BIT;
    b.dstStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    b.dstAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    b.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    b.newLayout = VK_IMAGE_LAYOUT_GENERAL;
    b.image = m_cloudNoise;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.imageMemoryBarrierCount = 1;
    dep.pImageMemoryBarriers = &b;
    vkCmdPipelineBarrier2(cmd, &dep);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cloudNoisePipeline);
    VkDescriptorImageInfo info{VK_NULL_HANDLE, m_cloudNoiseView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    w.dstBinding = 0;
    w.descriptorCount = 1;
    w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    w.pImageInfo = &info;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cloudNoiseLayout, 0, 1, &w);
    vkCmdDispatch(cmd, kNoiseSize / 4, kNoiseSize / 4, kNoiseSize / 4);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT;
    mb.dstAccessMask = VK_ACCESS_2_SHADER_SAMPLED_READ_BIT;
    dep = VkDependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
}

}  // namespace rvk
