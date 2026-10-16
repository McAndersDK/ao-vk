// Enhancement: volumetric clouds (RVK_CloudsOn). The game's clouds are puffs (DisplaySystem VisualSingleCloud_t: up to
// 30 to a cloud object, each a curved textured mesh - single_cloud01..04.png - with its own place, turn, colour and
// opacity), drawn with the sky at the start of the scene. Each puff's draw is taken instead of drawn (CaptureCloudPuff:
// its world matrix, its mesh's extent, how its texture lies on it, its colour and opacity); when the sky ends - the
// first draw after it - every puff is drawn as a volume in its place (DrawClouds, cloud.vert/.frag): its texture's
// silhouette given a body - thickest where the texture is most opaque - frayed by drifting 3D noise, lit by the sun
// through itself and by the sky. The game's placement and colours stay; the clouds gain depth and light.
#include "internal.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

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
const uint32_t kSunFragSpirv[] = {             // the suns (sun.cpp)
#include "sun.frag.inc"
};
const uint32_t kSunVisCompSpirv[] = {          // how much of each sun is seen (sun.cpp)
#include "sun_vis.comp.inc"
};
const uint32_t kTerrainSunFragSpirv[] = {      // the terrain's shade from a low sun (terrain_sun.cpp)
#include "terrain_sun.frag.inc"
};
const uint32_t kCloudShadowFragSpirv[] = {
#include "cloud_shadow.frag.inc"
};
const uint32_t kCloudLayerNoiseSpirv[] = {
#include "cloud_layer_noise.comp.inc"
};
const uint32_t kCloudWeatherFragSpirv[] = {
#include "cloud_weather.frag.inc"
};
const uint32_t kCloudLayerFragSpirv[] = {
#include "cloud_layer.frag.inc"
};
const uint32_t kCloudLayerShadowFragSpirv[] = {   // cloud_layer.frag built with RVK_CLOUD_SHADOW
#include "cloud_layer_shadow.frag.inc"
};
const uint32_t kCloudLayerSunRayFragSpirv[] = {   // ... with RVK_CLOUD_SUNRAY
#include "cloud_layer_sunray.frag.inc"
};
const uint32_t kCloudResolveFragSpirv[] = {
#include "cloud_resolve.frag.inc"
};

constexpr uint32_t kShadowSize = 1024;          // the clouds' shadow map (pixels a side; cloud_layer.frag kShadowMapSize)
constexpr float kShadowArea = 3000.0f;          // ... over this much of the world round the camera
constexpr float kLayerHeight = 350.0f;          // the cloud layer above the camera (world), where the puffs' shadows
                                                // are cast from (the game's sky draws them nearer than they are)
constexpr uint32_t kShadowPuffs = 64;

struct ShadowFrame {
    float area[4], info[4], drift[4], puff[kShadowPuffs][4];
};

constexpr uint32_t kNoiseSize = 64;             // the clouds' noise image (cells a side; tiling)
// RVK_CloudLayer.
constexpr uint32_t kShapeSize = 128, kDetailSize = 64;   // the layer's noise images (cells a side; tiling)
constexpr uint32_t kWeatherSize = 512;          // the weather map (pixels a side)
constexpr float kDeckThickness = 300.0f;        // the low deck (stratus): its thickness (world)
constexpr float kLayerShadowArea = 8000.0f;     // RVK_CloudLayer's shadow map: over this much (world) round the camera
                                                // (~8 units a texel: the clouds' edges and small clouds as patches on
                                                // the ground - at 47 a cloud's shadow covered all the land in view)
// The game's puff textures (RDB 1010004): single_cloud01..03 (121213, 121212, 121211) fibrous puffs, 04 streaky wisps.
constexpr uint32_t kCloudTexWispy = 121214;
constexpr float kWeatherArea = 64000.0f;        // ... over this much of the world round the camera (a storm's out
                                                // to kStormReach: 125 units a texel)
constexpr float kLayerThickness = 1300.0f;      // the layer's base to its top (world)
constexpr float kLayerRadius = 400000.0f;       // the sphere it bends round (the horizon's curve; 60 km sank the
                                                // far clouds below the eye's level, onto the hills)
constexpr float kLayerReach = 16000.0f;         // the march's farthest (world)
constexpr float kStormReach = 40000.0f;         // ... in a thunderstorm: its mass out to the horizon (the 400 km
                                                // sphere drops the base ~2000 units there - user: clouds only in a
                                                // circle round the player)
struct WeatherFrame {
    float area[4], info[4], drift[4], storm[4];
};

struct UpFrame {                                 // cloud_up.frag's block: the cloud layer's lightning
    d3d::Matrix invViewProj;
    float eye[4], tint[4], flash[4], flashWorld[4];
};

struct LayerFrame {
    d3d::Matrix invViewProj;
    float eye[4], layer[4], weather[4], shape[4], wind[4], tint[4], ambient[4], haze[4], view[4], sub[4], horizon[4],
        shadowArea[4], deck[4], scud[4], morph[4], quality[4];
};

struct ResolveFrame {
    d3d::Matrix invViewProj, prevViewProj;
    float size[4], sub[4];
};

// The view-projection of the camera's rotation alone (its translation dropped): directions to clip space.
float smoothstep01(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

d3d::Matrix RotationViewProj(const d3d::Matrix& view, const d3d::Matrix& proj)
{
    d3d::Matrix v = view;
    v.m[3][0] = v.m[3][1] = v.m[3][2] = 0.0f;
    return MulMatrix(v, proj);
}
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
    // The half size's upsampling: 0 the clouds (linear), 1 its block (UpFrame: a lightning flash); push: the scene's size.
    {
        const VkDescriptorSetLayoutBinding u[2] = {
            {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr},
            {1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}};   // (the flash: UpFrame)
        VkDescriptorSetLayoutCreateInfo ul{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ul.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ul.bindingCount = 2;
        ul.pBindings = u;
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
    const bool v1 = pipeline(kCloudVertSpirv, sizeof(kCloudVertSpirv), kCloudFragSpirv, sizeof(kCloudFragSpirv), m_cloudLayout,
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
    if (!v1)
        return false;
    {   // The sun's disc (sun.cpp): in the scene's rendering, added (over with alpha 0); a push-only layout.
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 128};
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_sunLayout), "sun layout", error) ||
            !pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kSunFragSpirv, sizeof(kSunFragSpirv), m_sunLayout,
                      sceneFormats, 5, kDepthFormat, &m_sunPipeline))
            return false;
        // How much of each is seen (sun_vis.comp): depth, clouds, the result (pushed); the result read by the CPU.
        const VkShaderStageFlags comp = VK_SHADER_STAGE_COMPUTE_BIT;
        const VkDescriptorSetLayoutBinding vb[3] = {{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
                                                    {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, comp, nullptr},
                                                    {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, comp, nullptr}};
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = 3;
        li.pBindings = vb;
        VkPushConstantRange vp{comp, 0, 80};
        VkPipelineLayoutCreateInfo vl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        vl.setLayoutCount = 1;
        vl.pSetLayouts = &m_sunVisSetLayout;
        vl.pushConstantRangeCount = 1;
        vl.pPushConstantRanges = &vp;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &li, nullptr, &m_sunVisSetLayout), "sun visibility set", error) ||
            !Check(vkCreatePipelineLayout(m_device, &vl, nullptr, &m_sunVisLayout), "sun visibility layout", error))
            return false;
        VkShaderModule m;
        if (!module(kSunVisCompSpirv, sizeof(kSunVisCompSpirv), &m))
            return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = m_sunVisLayout;
        const bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_sunVisPipeline),
                              "sun visibility pipeline", error);
        vkDestroyShaderModule(m_device, m, nullptr);
        if (!ok)
            return false;
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        m_sunVisStride = std::max<VkDeviceSize>(256, m_props.limits.minStorageBufferOffsetAlignment);
        bi.size = m_sunVisStride * kFramesInFlight;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO;
        ac.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
        VmaAllocationInfo info{};
        if (vmaCreateBuffer(m_allocator, &bi, &ac, &m_sunVisBuffer, &m_sunVisAllocation, &info) != VK_SUCCESS)
            return false;
        m_sunVisCpu = static_cast<uint8_t*>(info.pMappedData);
        // The terrain's shade from a low sun (terrain_sun.cpp): heights (a pushed storage buffer) -> RG16F, written.
        const VkDescriptorSetLayoutBinding tb{0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo tl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        tl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        tl.bindingCount = 1;
        tl.pBindings = &tb;
        VkPushConstantRange tp{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};
        VkPipelineLayoutCreateInfo tpl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        tpl.setLayoutCount = 1;
        tpl.pSetLayouts = &m_terrainSunSetLayout;
        tpl.pushConstantRangeCount = 1;
        tpl.pPushConstantRanges = &tp;
        const VkFormat terrainFormat = GetFormatInfo(Format::RG16F).vk;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &tl, nullptr, &m_terrainSunSetLayout), "terrain sun set", error) ||
            !Check(vkCreatePipelineLayout(m_device, &tpl, nullptr, &m_terrainSunLayout), "terrain sun layout", error) ||
            !pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kTerrainSunFragSpirv, sizeof(kTerrainSunFragSpirv),
                      m_terrainSunLayout, &terrainFormat, 1, VK_FORMAT_UNDEFINED, &m_terrainSunPipeline))
            return false;
    }

    // RVK_CloudLayer: its noise (two 3D images, one compute pipeline: push mode, size), the weather map's pass (0 its
    // block, 1 v1's noise), the march (0 its block, 1 the weather map, 2 the shape noise, 3 the pre-pass's depth,
    // 4 the frame lights, 5 the detail noise).
    auto volume = [&](uint32_t size, CloudVolume& v) {
        VkImageCreateInfo ic{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        ic.imageType = VK_IMAGE_TYPE_3D;
        ic.format = VK_FORMAT_R8G8B8A8_UNORM;
        ic.extent = {size, size, size};
        ic.mipLevels = 1;
        ic.arrayLayers = 1;
        ic.samples = VK_SAMPLE_COUNT_1_BIT;
        ic.tiling = VK_IMAGE_TILING_OPTIMAL;
        ic.usage = VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateImage(m_allocator, &ic, &ac, &v.image, &v.allocation, nullptr), "cloud layer noise", error))
            return false;
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = v.image;
        vi.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vi.format = ic.format;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        return Check(vkCreateImageView(m_device, &vi, nullptr, &v.view), "cloud layer noise view", error);
    };
    if (!volume(kShapeSize, m_cloudShape) || !volume(kDetailSize, m_cloudDetail))
        return false;
    m_cloudLayerNoiseMade = false;
    {
        VkPushConstantRange push{VK_SHADER_STAGE_COMPUTE_BIT, 0, 8};
        VkPipelineLayoutCreateInfo nl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        nl.setLayoutCount = 1;
        nl.pSetLayouts = &m_cloudNoiseSetLayout;         // (one storage image, as v1's noise)
        nl.pushConstantRangeCount = 1;
        nl.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(m_device, &nl, nullptr, &m_cloudLayerNoiseLayout), "cloud layer noise layout", error))
            return false;
        VkShaderModule m;
        if (!module(kCloudLayerNoiseSpirv, sizeof(kCloudLayerNoiseSpirv), &m))
            return false;
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, m, "main", nullptr};
        ci.layout = m_cloudLayerNoiseLayout;
        const bool ok = Check(vkCreateComputePipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_cloudLayerNoisePipeline),
                              "cloud layer noise pipeline", error);
        vkDestroyShaderModule(m_device, m, nullptr);
        if (!ok)
            return false;
    }
    auto setLayout = [&](const VkDescriptorSetLayoutBinding* bindings, uint32_t count, VkDescriptorSetLayout* set,
                         VkPipelineLayout* layout) {
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        li.bindingCount = count;
        li.pBindings = bindings;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &li, nullptr, set), "cloud layer set", error))
            return false;
        VkPipelineLayoutCreateInfo pi{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pi.setLayoutCount = 1;
        pi.pSetLayouts = set;
        return Check(vkCreatePipelineLayout(m_device, &pi, nullptr, layout), "cloud layer layout", error);
    };
    const VkShaderStageFlags frag = VK_SHADER_STAGE_FRAGMENT_BIT;
    const VkDescriptorSetLayoutBinding wb[2] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, frag, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr}};
    const VkDescriptorSetLayoutBinding lb[7] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, frag, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr},
                                                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr},
                                                {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr},
                                                {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, frag, nullptr},
                                                {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr},
                                                {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr}};
    const VkFormat weatherFormat = GetFormatInfo(Format::RGBA8).vk;
    const VkDescriptorSetLayoutBinding rb[3] = {{0, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, frag, nullptr},
                                                {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr},
                                                {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, frag, nullptr}};
    return setLayout(wb, 2, &m_cloudWeatherSetLayout, &m_cloudWeatherLayout) &&
           setLayout(rb, 3, &m_cloudResolveSetLayout, &m_cloudResolveLayout) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudResolveFragSpirv, sizeof(kCloudResolveFragSpirv),
                    m_cloudResolveLayout, &halfFormat, 1, VK_FORMAT_UNDEFINED, &m_cloudResolvePipeline) &&
           setLayout(lb, 7, &m_cloudLayerSetLayout, &m_cloudLayerLayout) &&
           // (Written whole: blended over with alpha 1 / over a cleared target.)
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudWeatherFragSpirv, sizeof(kCloudWeatherFragSpirv),
                    m_cloudWeatherLayout, &weatherFormat, 1, VK_FORMAT_UNDEFINED, &m_cloudWeatherPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudLayerFragSpirv, sizeof(kCloudLayerFragSpirv),
                    m_cloudLayerLayout, &halfFormat, 1, VK_FORMAT_UNDEFINED, &m_cloudLayerPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudLayerShadowFragSpirv,
                    sizeof(kCloudLayerShadowFragSpirv), m_cloudLayerLayout, &shadowFormat, 1, VK_FORMAT_UNDEFINED,
                    &m_cloudLayerShadowPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kCloudLayerSunRayFragSpirv,
                    sizeof(kCloudLayerSunRayFragSpirv), m_cloudLayerLayout, &shadowFormat, 1, VK_FORMAT_UNDEFINED,
                    &m_cloudSunRayPipeline);
}

// Blue noise (void and cluster, Ulichney 1993): 64 x 64 ranks whose every threshold is evenly spread - no clumps, no
// gaps - so the cloud march's start offsets differ between neighbours as much as they can (interleaved gradient noise
// left its diagonal lines in the clouds' edges: banding - vertexfragment.com "volumetric cloud banding"). Made once at
// start (~20 ms); r the ranks, g the same half a tile over (the sun march's).
Texture* Device::CreateBlueNoise()
{
    const auto begin = std::chrono::steady_clock::now();
    constexpr int kN = 64, kAll = kN * kN;
    // A gaussian (sigma 1.9) on the torus, as offsets.
    std::vector<float> g(kAll);
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) {
            const int dx = std::min(x, kN - x), dy = std::min(y, kN - y);
            g[y * kN + x] = std::exp(-float(dx * dx + dy * dy) / (2.0f * 1.9f * 1.9f));
        }
    std::vector<uint8_t> on(kAll, 0);
    std::vector<float> energy(kAll, 0.0f);
    auto toggle = [&](int i, bool set) {
        on[size_t(i)] = set ? 1 : 0;
        const int ix = i % kN, iy = i / kN;
        const float sign = set ? 1.0f : -1.0f;
        for (int y = 0; y < kN; ++y) {
            const float* row = &g[size_t(((y - iy + kN) % kN) * kN)];
            float* e = &energy[size_t(y * kN)];
            for (int x = 0; x < kN; ++x) e[x] += sign * row[(x - ix + kN) % kN];
        }
    };
    auto tightest = [&](uint8_t want) {          // (the most crowded of the set, or the emptiest of the unset)
        int best = -1;
        for (int i = 0; i < kAll; ++i)
            if (on[size_t(i)] == want && (best < 0 || (want ? energy[size_t(i)] > energy[size_t(best)] : energy[size_t(i)] < energy[size_t(best)])))
                best = i;
        return best;
    };
    // A random tenth set, then evened out: the most crowded moved to the emptiest until it stays.
    uint32_t seed = 0x9E3779B9u;
    auto rnd = [&]() { seed ^= seed << 13; seed ^= seed >> 17; seed ^= seed << 5; return seed; };
    const int initial = kAll / 10;
    for (int k = 0; k < initial;) {
        const int i = int(rnd() % uint32_t(kAll));
        if (!on[size_t(i)]) { toggle(i, true); ++k; }
    }
    for (int guard = 0; guard < kAll; ++guard) {
        const int c = tightest(1);
        toggle(c, false);
        const int v = tightest(0);
        if (v == c) { toggle(c, true); break; }
        toggle(v, true);
    }
    const std::vector<uint8_t> start = on;
    const std::vector<float> startEnergy = energy;
    std::vector<int> rank(kAll, 0);
    // The initial points ranked down (the most crowded taken away last-first), the rest ranked up into the emptiest.
    for (int r = initial - 1; r >= 0; --r) {
        const int c = tightest(1);
        rank[size_t(c)] = r;
        toggle(c, false);
    }
    on = start;
    energy = startEnergy;
    for (int r = initial; r < kAll; ++r) {
        const int v = tightest(0);
        rank[size_t(v)] = r;
        toggle(v, true);
    }
    std::vector<uint32_t> px(kAll);
    for (int y = 0; y < kN; ++y)
        for (int x = 0; x < kN; ++x) {
            const uint32_t r = uint32_t(rank[size_t(y * kN + x)] * 256 / kAll);
            const uint32_t s2 = uint32_t(rank[size_t(((y + kN / 2) % kN) * kN + (x + kN / 2 + 7) % kN)] * 256 / kAll);
            px[size_t(y * kN + x)] = 0xFF000000u | (r << 16) | (s2 << 8);
        }
    Log("startup: blue noise %d ms", int(std::chrono::duration_cast<std::chrono::milliseconds>(
                                          std::chrono::steady_clock::now() - begin).count()));
    return CreateTexture(kN, kN, px.data());
}

void Device::DestroyCloudResources()
{
    if (m_cloudSunRay) { DestroyTexture(m_cloudSunRay); m_cloudSunRay = nullptr; }
    if (m_terrainSunMap) { DestroyTexture(m_terrainSunMap); m_terrainSunMap = nullptr; }
    if (m_terrainSunPipeline) { vkDestroyPipeline(m_device, m_terrainSunPipeline, nullptr); m_terrainSunPipeline = VK_NULL_HANDLE; }
    if (m_terrainSunLayout) { vkDestroyPipelineLayout(m_device, m_terrainSunLayout, nullptr); m_terrainSunLayout = VK_NULL_HANDLE; }
    if (m_terrainSunSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_terrainSunSetLayout, nullptr); m_terrainSunSetLayout = VK_NULL_HANDLE; }
    if (m_sunPipeline) { vkDestroyPipeline(m_device, m_sunPipeline, nullptr); m_sunPipeline = VK_NULL_HANDLE; }
    if (m_sunVisPipeline) { vkDestroyPipeline(m_device, m_sunVisPipeline, nullptr); m_sunVisPipeline = VK_NULL_HANDLE; }
    if (m_sunVisLayout) { vkDestroyPipelineLayout(m_device, m_sunVisLayout, nullptr); m_sunVisLayout = VK_NULL_HANDLE; }
    if (m_sunVisSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_sunVisSetLayout, nullptr); m_sunVisSetLayout = VK_NULL_HANDLE; }
    if (m_sunVisBuffer) { vmaDestroyBuffer(m_allocator, m_sunVisBuffer, m_sunVisAllocation); m_sunVisBuffer = VK_NULL_HANDLE; }
    m_sunVisCpu = nullptr;
    if (m_sunLayout) { vkDestroyPipelineLayout(m_device, m_sunLayout, nullptr); m_sunLayout = VK_NULL_HANDLE; }
    for (VkPipeline* p : {&m_cloudPipeline, &m_cloudHalfPipeline, &m_cloudUpPipeline, &m_cloudUpScenePipeline,
                          &m_cloudShadowPipeline, &m_cloudLayerNoisePipeline, &m_cloudWeatherPipeline, &m_cloudLayerPipeline,
                          &m_cloudResolvePipeline, &m_cloudLayerShadowPipeline, &m_cloudSunRayPipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    for (VkPipelineLayout* l : {&m_cloudLayerNoiseLayout, &m_cloudWeatherLayout, &m_cloudLayerLayout, &m_cloudResolveLayout})
        if (*l) { vkDestroyPipelineLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    for (VkDescriptorSetLayout* l : {&m_cloudWeatherSetLayout, &m_cloudLayerSetLayout, &m_cloudResolveSetLayout})
        if (*l) { vkDestroyDescriptorSetLayout(m_device, *l, nullptr); *l = VK_NULL_HANDLE; }
    for (CloudVolume* v : {&m_cloudShape, &m_cloudDetail}) {
        if (v->view) vkDestroyImageView(m_device, v->view, nullptr);
        if (v->image) vmaDestroyImage(m_allocator, v->image, v->allocation);
        *v = CloudVolume{};
    }
    if (m_cloudWeather) { DestroyTexture(m_cloudWeather); m_cloudWeather = nullptr; }
    for (Texture** t : {&m_cloudQuarter, &m_cloudHistory[0], &m_cloudHistory[1]})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    m_cloudHistoryValid = false;
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
        return !CloudSheetsOn();                  // (hidden all the same - or drawn, a sheet)
    const FvfLayout layout = DecodeFvf(fvf);
    const uint8_t* base = static_cast<const uint8_t*>(vertices);
    CloudPuff p{};
    p.world = m_world;
    p.view = m_view;
    p.proj = m_proj;
    p.texture = m_textures[0];
    p.rdbId = p.texture ? p.texture->m_rdbId : 0;
    for (int k = 0; k < 3; ++k) { p.low[k] = 1e30f; p.high[k] = -1e30f; }
    // Least squares: u, v = a . (x, y, z, 1) - the normal equations' sums.
    double ata[4][4] = {}, atu[4] = {}, atv[4] = {};
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
    // Its colour as the game lit it - D3D's fixed-function lighting, per vertex, averaged: the emissive and the
    // global ambient (times their material sources) plus each directional light's diffuse by the vertex normal (times
    // the diffuse source), clamped at 1 as D3D does; its opacity the diffuse source's alpha (the vertex alpha: the game
    // fades puffs in and out through it). In storms the vertex colours are dark (~0.25) but the emissive and the
    // ambient keep the puffs lighter than the sky - scaling everything by the vertex colour made them vanish.
    {
        const bool colourVertex = m_rs[d3d::RS_COLORVERTEX] != 0;
        auto source = [&](uint32_t rs, const d3d::Color& material, const float* c1, const float* c2, float out[4]) {
            const uint32_t src = colourVertex ? m_rs[rs] : d3d::MCS_MATERIAL;
            const float* v = src == d3d::MCS_COLOR1 ? c1 : src == d3d::MCS_COLOR2 ? c2 : nullptr;
            if (v) for (int k = 0; k < 4; ++k) out[k] = v[k];
            else out[0] = material.r, out[1] = material.g, out[2] = material.b, out[3] = material.a;
        };
        const uint32_t amb = m_rs[d3d::RS_AMBIENT];
        const float global[3] = {((amb >> 16) & 0xFF) / 255.0f, ((amb >> 8) & 0xFF) / 255.0f, (amb & 0xFF) / 255.0f};
        const bool lit = m_rs[d3d::RS_LIGHTING] != 0, hasNormal = layout.offset[1] >= 0;
        const bool hasSpecular = layout.offset[3] >= 0;
        double sum[4] = {};
        for (uint32_t i = 0; i < vertexCount; ++i) {
            const uint8_t* v = base + size_t(i) * layout.stride;
            float c1[4] = {1, 1, 1, 1}, c2[4] = {1, 1, 1, 1};   // (a missing vertex colour: white, as D3D's default)
            auto unpack = [](const uint8_t* at, float out[4]) {
                uint32_t argb;
                std::memcpy(&argb, at, 4);
                out[0] = ((argb >> 16) & 0xFF) / 255.0f;
                out[1] = ((argb >> 8) & 0xFF) / 255.0f;
                out[2] = (argb & 0xFF) / 255.0f;
                out[3] = (argb >> 24) / 255.0f;
            };
            if (hasColour) unpack(v + layout.offset[2], c1);
            if (hasSpecular) unpack(v + layout.offset[3], c2);
            float diffuse[4], ambientSrc[4], emissive[4];
            source(d3d::RS_DIFFUSEMATERIALSOURCE, m_material.diffuse, c1, c2, diffuse);
            source(d3d::RS_AMBIENTMATERIALSOURCE, m_material.ambient, c1, c2, ambientSrc);
            source(d3d::RS_EMISSIVEMATERIALSOURCE, m_material.emissive, c1, c2, emissive);
            float out[3];
            if (!lit) {
                for (int k = 0; k < 3; ++k) out[k] = c1[k];
            } else {
                for (int k = 0; k < 3; ++k) out[k] = emissive[k] + global[k] * ambientSrc[k];
                float n[3] = {0.0f, 1.0f, 0.0f};
                if (hasNormal) {
                    float nm[3];
                    std::memcpy(nm, v + layout.offset[1], sizeof(nm));
                    for (int j = 0; j < 3; ++j)
                        n[j] = nm[0] * p.world.m[0][j] + nm[1] * p.world.m[1][j] + nm[2] * p.world.m[2][j];
                    const float l = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                    if (l > 1e-6f) for (float& x : n) x /= l;
                }
                for (const LightSlot& slot : m_lights) {
                    if (!slot.enabled || slot.light.type != d3d::LIGHT_DIRECTIONAL)
                        continue;
                    if (i == 0)
                        NoteCloudGameLight(slot.light.diffuse);   // (the game's sunlight on its clouds: its hue)
                    const d3d::Vector& d = slot.light.direction;
                    const float dl = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
                    if (dl < 1e-6f) continue;
                    const float nl = std::max(-(n[0] * d.x + n[1] * d.y + n[2] * d.z) / dl, 0.0f);
                    out[0] += diffuse[0] * slot.light.diffuse.r * nl + ambientSrc[0] * slot.light.ambient.r;
                    out[1] += diffuse[1] * slot.light.diffuse.g * nl + ambientSrc[1] * slot.light.ambient.g;
                    out[2] += diffuse[2] * slot.light.diffuse.b * nl + ambientSrc[2] * slot.light.ambient.b;
                }
            }
            for (int k = 0; k < 3; ++k) sum[k] += std::clamp(out[k], 0.0f, 1.0f);
            sum[3] += lit ? diffuse[3] : c1[3];
        }
        for (int k = 0; k < 4; ++k) p.colour[k] = float(sum[k] / std::max(vertexCount, 1u));
    }
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
    return !CloudSheetsOn();                     // (RVK_CloudSheet: drawn, a sheet behind the layer - DrawTransform)
}

// The puffs taken this frame, drawn as volumes where the sky ends (no depth test: only the sky is there yet). Far ones
// first.
//
// RVK_CloudHalf marches them into a half-size target and filters that up over the scene's targets; RVK_CloudShadow
// makes their shadow map first. Both need their own renderings - outside the scene's. With the depth pre-pass armed
// (prepass.cpp: the scene's rendering split at its depth clear) they are recorded into their own commands, run after
// the pre-pass and before the scene's draws (Frame::clouds): the scene's rendering - and its pre-pass segment - go on
// unbroken, and the march skips every pixel the pre-pass found an opaque surface at (the scene's draws cover it
// The game's sunlight on one of its puffs (CaptureCloudPuff): the brightest this frame kept; a new frame takes the
// last one's as the clouds' (eased hue: about half a second).
void Device::NoteCloudGameLight(const d3d::Color& c)
{
    if (m_cloudGameLightFrame != m_frameNumber) {
        std::memcpy(m_cloudGameLight, m_cloudGameLightNext, sizeof(m_cloudGameLight));
        m_cloudGameLightNext[0] = m_cloudGameLightNext[1] = m_cloudGameLightNext[2] = 0.0f;
        m_cloudGameLightFrame = m_frameNumber;
        const float mx = std::max({m_cloudGameLight[0], m_cloudGameLight[1], m_cloudGameLight[2]});
        if (mx > 0.02f) {
            const float k = m_cloudGameHueFrame == ~0ull ? 1.0f : 0.03f;
            for (int j = 0; j < 3; ++j) m_cloudGameHue[j] += (m_cloudGameLight[j] / mx - m_cloudGameHue[j]) * k;
            m_cloudGameHueFrame = m_frameNumber;
        }
    }
    const float luma = 0.3f * c.r + 0.59f * c.g + 0.11f * c.b;
    if (luma > 0.3f * m_cloudGameLightNext[0] + 0.59f * m_cloudGameLightNext[1] + 0.11f * m_cloudGameLightNext[2]) {
        m_cloudGameLightNext[0] = std::max(c.r, 0.0f);
        m_cloudGameLightNext[1] = std::max(c.g, 0.0f);
        m_cloudGameLightNext[2] = std::max(c.b, 0.0f);
    }
}

// later). Without the pre-pass, the scene's rendering is ended for them and begun again (and nothing is skipped).
void Device::DrawClouds(VkCommandBuffer cmd)
{
    m_cloudsDrawn = true;
    if (m_cloudPuffs.empty() || !m_cloudPipeline || !m_rendering)
        return;
    FlushGroup();
    ProfileSceneClass(0);
    EnsureRingSpace(m_cloudPuffs.size() * (sizeof(CloudFrame) + 256) + sizeof(FrameLights) + sizeof(ShadowFrame) +
                    sizeof(WeatherFrame) + sizeof(LayerFrame) + 4096);
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
    // RVK_CloudLayer: one layer over the sky instead of a volume per puff (the puffs say where: the weather map); always
    // at half size.
    const bool layer = m_cloudParams.layer && m_cloudLayerPipeline && m_cloudWeatherPipeline && m_cloudLayerNoiseMade;
    if ((m_cloudParams.half || layer) && (!m_cloudHalf || m_cloudHalf->m_width != hw || m_cloudHalf->m_height != hh)) {
        if (m_cloudHalf) DestroyTexture(m_cloudHalf);
        m_cloudHalf = CreateImage(hw, hh, Format::RGBA16F, 1, true);
    }
    const bool half = (m_cloudParams.half || layer) && m_cloudHalf && m_cloudHalfPipeline && m_cloudUpPipeline &&
                      m_cloudUpScenePipeline;
    const d3d::Matrix& camera = m_aoProjValid ? m_aoView : m_view;   // (the world camera, as the layer's march)
    float eyeNow[3];
    for (int i = 0; i < 3; ++i)
        eyeNow[i] = -(camera.m[3][0] * camera.m[i][0] + camera.m[3][1] * camera.m[i][1] + camera.m[3][2] * camera.m[i][2]);
    UpdateCloudDrift();
    if (layer) {   // The layer's base: RVK_CloudHeight above the eye's ground, following its height slowly.
        const float target = eyeNow[1] + m_cloudParams.height;
        const double gap = m_cloudBaseClock < 0.0 ? 1e9 : m_frameClock - m_cloudBaseClock;
        m_cloudBase = gap > 5.0 ? target : m_cloudBase + (target - m_cloudBase) * (1.0f - std::exp(-float(gap) / 20.0f));
        m_cloudBaseClock = m_frameClock;
    }
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
    if (layer)
        MakeCloudWeather(work, eyeNow);          // (the layer's shadow and march read it)
    if (layer && (own || half || shadow))
        MakeCloudSunRay(work);                   // (outside any rendering: the sun's light through the clouds)
    if (shadow) {
        if (layer)
            MakeCloudLayerShadow(work, eyeNow);
        else
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
        float lo[2] = {0.0f, 0.0f}, hi[2] = {float(fw), float(fh)};
        Texture* clouds = m_cloudHalf;           // the half-size clouds to filter up
        if (layer) {
            clouds = ReusedCloudLayer(work, skipCovered, eyeNow);
            if (skipCovered)
                MeasureSunVisibility(work, clouds);   // (sun.cpp: the glare follows how much of each sun is seen)
        } else {
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
            MarchCloudPuffs(work, order, true, skipCovered, lo, hi);
            vkCmdEndRendering(work);
            Transition(work, m_cloudHalf, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        }
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
            VkDescriptorImageInfo image{m_cloudClampSampler, clouds->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
            // (The cloud layer's lightning: added here, not marched - cloud_up.frag.)
            UpFrame uf{};
            {
                float flash[4], view[4], proj[4], world[4];
                Lightning(flash, view, proj, world);
                if (layer && m_cloudUpFrameNumber == m_frameNumber && flash[3] > 0.0f) {
                    uf.invViewProj = m_cloudUpInvViewProj;
                    std::memcpy(uf.eye, m_cloudUpEye, sizeof(uf.eye));
                    std::memcpy(uf.tint, m_cloudUpTint, sizeof(uf.tint));
                    uf.flash[3] = m_cloudUpReach;
                    const float rgb[3] = {0.8f, 0.85f, 1.0f};   // (bluish white: as the march's FlashInCloud was)
                    for (int i = 0; i < 3; ++i) uf.flash[i] = rgb[i] * flash[3] * 1.2f;
                    for (int i = 0; i < 3; ++i) uf.flashWorld[i] = world[i];
                }
            }
            void* ucpu = nullptr;
            const VkDeviceSize uoff = Allocate(sizeof(UpFrame), 256, &ucpu);
            std::memcpy(ucpu, &uf, sizeof(uf));
            VkDescriptorBufferInfo ubuf{m_frames[m_frameIndex].ring, uoff, sizeof(UpFrame)};
            VkWriteDescriptorSet w[2] = {{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}, {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET}};
            w[0].dstBinding = 0;
            w[0].descriptorCount = 1;
            w[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
            w[0].pImageInfo = &image;
            w[1].dstBinding = 1;
            w[1].descriptorCount = 1;
            w[1].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[1].pBufferInfo = &ubuf;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudUpLayout, 0, 2, w);
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
        // The box: the sheet given a body across its thin axis - a curved puff half as thick as it is wide, a flat sheet
        // (the game's storm layers: hundreds of units across, none thick) a sixth - but never reaching the eye: the
        // sky's sheets can lie near the eye's height in its space, and a body round the eye is a wall of cloud with
        // the box's sides as hard edges.
        float low[3], high[3];
        std::memcpy(low, p.low, sizeof(low));
        std::memcpy(high, p.high, sizeof(high));
        const int a = p.thin == 0 ? 1 : 0, b = p.thin == 2 ? 1 : 2;
        const float width = std::min(high[a] - low[a], high[b] - low[b]);
        const float mid = 0.5f * (low[p.thin] + high[p.thin]);
        const float sheet = 0.5f * (high[p.thin] - low[p.thin]);
        const bool flat = sheet < 0.02f * width;
        float thick = std::max(sheet, (flat ? 0.08f : 0.25f) * width);
        thick = std::max(std::min(thick, 0.5f * std::fabs(cf.eye[p.thin] - mid)), std::max(sheet, 1e-3f * width));
        low[p.thin] = mid - thick;
        high[p.thin] = mid + thick;
        std::memcpy(cf.low, low, sizeof(low));
        std::memcpy(cf.high, high, sizeof(high));
        cf.low[3] = float(p.thin);
        cf.high[3] = float(std::clamp(m_cloudParams.steps, 8u, 96u));
        std::memcpy(cf.uvU, p.uvU, sizeof(cf.uvU));
        std::memcpy(cf.uvV, p.uvV, sizeof(cf.uvV));
        // Rain (rvk rain.cpp: how hard, eased): the clouds heavier - darker, greyer, denser.
        const float storm = StormSky();
        const float grey = (p.colour[0] + p.colour[1] + p.colour[2]) / 3.0f;
        for (int k = 0; k < 3; ++k)
            cf.colour[k] = (p.colour[k] + (grey - p.colour[k]) * 0.6f * storm) * (1.0f - 0.25f * storm) * m_cloudParams.brightness *
                           (1.0f - 0.35f * std::clamp(m_stormGreen, 0.0f, 1.0f) * storm);
        StormHue(cf.colour);                     // (a thunderstorm's: dark yellow-green - RVK_StormGreen)
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
    sf.info[2] = std::clamp(cover * 0.6f + StormSky() * 0.5f, 0.0f, 1.0f) *
                 std::min(m_cloudParams.density, 1.5f);
    sf.info[2] = std::min(sf.info[2], 1.0f);
    sf.info[3] = area * 0.2f;                    // (a few cloud shadows across the map)
    // Drifting with the clouds (UpdateCloudDrift).
    sf.drift[0] = m_cloudDrift[0];
    sf.drift[1] = m_cloudDrift[1];
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

// How far the clouds have drifted (world x, z) and evolved (the noise's y), added up frame by frame: the wind's way
// (the rain's, else a slow breeze) eased over seconds, 8 world units a second. (Its direction times the time since
// the start, as before, swung the whole field by kilometres whenever the game's gusty wind turned.)
void Device::UpdateCloudDrift()
{
    const double now = m_frameClock;
    const float dt = m_cloudDriftClock < 0.0 ? 0.0f : float(std::clamp(now - m_cloudDriftClock, 0.0, 0.25));
    m_cloudDriftClock = now;
    // The way: the game's rain's wind when it rains, else the sky's (sky.cpp: turning over hours; RVK_SkyVary 0: fixed).
    // The speed by the sky (sky weather phase 2): calm when clear, ~8 scattered, ~14 overcast, more in rain.
    const float vary = std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
    float wx = m_rainWind[0], wz = m_rainWind[2];
    const float wl = std::sqrt(wx * wx + wz * wz);
    if (wl >= 0.1f) {
        wx /= wl; wz /= wl;
    } else if (vary > 0.0f) {
        float sw[2];
        SkyWind(sw);
        wx = 0.8f + (sw[0] - 0.8f) * vary; wz = 0.6f + (sw[1] - 0.6f) * vary;
    } else {
        wx = 0.8f; wz = 0.6f;
    }
    const float k = 1.0f - std::exp(-dt / 10.0f);
    m_cloudWindDir[0] += (wx - m_cloudWindDir[0]) * k;
    m_cloudWindDir[1] += (wz - m_cloudWindDir[1]) * k;
    const float speed = 8.0f + ((4.0f + 10.0f * SkyCloudiness()) - 8.0f) * vary + 6.0f * StormSky() * vary;
    m_cloudWindSpeed += (speed - m_cloudWindSpeed) * (1.0f - std::exp(-dt / 30.0f));
    const float dl = std::max(std::sqrt(m_cloudWindDir[0] * m_cloudWindDir[0] + m_cloudWindDir[1] * m_cloudWindDir[1]), 1e-3f);
    m_cloudDrift[0] -= m_cloudWindDir[0] / dl * m_cloudWindSpeed * dt;
    m_cloudDrift[1] -= m_cloudWindDir[1] / dl * m_cloudWindSpeed * dt;
    m_cloudDrift[2] += 1.5f * dt;
    // The clouds' churn (cloud_layer.frag Density): a slow field through which they are pushed about, itself moving on -
    // changed wholly in ~90 s on a fair day, ~35 s in a storm (user: realistic, a storm's faster - at 20 s with a 7 km
    // field pushing ~1350 units, a lava lamp); its detail rising
    // faster in a storm (boiling); the weather map's masses growing and breaking up twice as fast (4x: holes opening and
    // closing in seconds).
    const float morph = std::clamp(m_cloudParams.morph, 0.0f, 3.0f), storm = StormSky();
    m_cloudMorphPhase = std::fmod(m_cloudMorphPhase + morph * (0.25f / (90.0f + (35.0f - 90.0f) * storm)) * dt, 1.0f);
    m_cloudBoil = std::fmod(m_cloudBoil + morph * 8.0f * storm * dt, 100000.0f);
    m_cloudWeatherTime = std::fmod(m_cloudWeatherTime + double((1.0f + storm * std::min(morph, 1.0f)) * dt), 36000.0);
    // Scud: in a thunderstorm the low deck races along under the layer, three times as fast and more (the storm's
    // gusts) - added up, so a change of pace never jumps it.
    const float scudSpeed = m_cloudWindSpeed * 2.5f * StormSky();
    m_scudDrift[0] = std::fmod(m_scudDrift[0] - m_cloudWindDir[0] / dl * scudSpeed * dt, 200000.0f);
    m_scudDrift[1] = std::fmod(m_scudDrift[1] - m_cloudWindDir[1] / dl * scudSpeed * dt, 200000.0f);
}

// RVK_CloudLayer: the weather map round the camera - the game's sky's cloudiness as a drifting field (fixed in the
// world), the cloud type and density from the weather (rain: storm clouds; the game's flat storm sheets: layered
// cloud). Outside any rendering.
void Device::MakeCloudWeather(VkCommandBuffer cmd, const float eye[3])
{
    if (!m_cloudWeather) {
        m_cloudWeather = CreateImage(kWeatherSize, kWeatherSize, Format::RGBA8, 1, true);
        if (!m_cloudWeather)
            return;
    }
    const float area = kWeatherArea, texel = area / float(kWeatherSize);
    WeatherFrame wf{};
    wf.area[0] = std::floor((eye[0] - area * 0.5f) / texel) * texel;
    wf.area[1] = std::floor((eye[2] - area * 0.5f) / texel) * texel;
    wf.area[2] = area;
    wf.area[3] = float(kWeatherSize);
    const float rain = StormSky();
    float cover = 0.0f, flat = 0.0f, weight = 0.0f, wispy = 0.0f;
    uint32_t known = 0;                          // (puffs whose texture is known)
    for (const CloudPuff& p : m_cloudPuffs) {
        float mid[3], w[3];
        for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (p.low[k] + p.high[k]);
        for (int j = 0; j < 3; ++j)
            w[j] = mid[0] * p.world.m[0][j] + mid[1] * p.world.m[1][j] + mid[2] * p.world.m[2][j] + p.world.m[3][j];
        const float d[3] = {w[0] - eye[0], w[1] - eye[1], w[2] - eye[2]};
        const float len = std::max(std::sqrt(d[0] * d[0] + d[1] * d[1] + d[2] * d[2]), 1.0f);
        float extent = 0.0f;
        for (int k = 0; k < 3; ++k) {
            if (k == p.thin) continue;
            const float* row = p.world.m[k];
            extent = std::max(extent, (p.high[k] - p.low[k]) * std::sqrt(row[0] * row[0] + row[1] * row[1] + row[2] * row[2]));
        }
        const float opacity = std::clamp(p.colour[3], 0.0f, 1.0f);
        const float angular = extent / len;
        if (p.rdbId == kCloudTexWispy) wispy += opacity;
        if (p.rdbId) ++known;
        cover += opacity * angular * angular * 0.5f;
        weight += opacity;
        {   // (a flat sheet: no thickness for its width - in its mesh's units)
            const int a = p.thin == 0 ? 1 : 0, b = p.thin == 2 ? 1 : 2;
            const float width = std::min(p.high[a] - p.low[a], p.high[b] - p.low[b]);
            if (0.5f * (p.high[p.thin] - p.low[p.thin]) < 0.02f * width) flat += opacity;
        }
    }
    const float flatShare = weight > 0.0f ? flat / weight : 0.0f;
    wf.info[0] = float(m_cloudWeatherTime);        // (its own clock: faster in a storm)
    // How cloudy: the game draws the same ~30 flat sheets with the same fibrous textures in every sky - fair days and
    // its storms alike (their size, count, opacity and fog share don't tell them apart: dumps of both) - and those
    // read as scattered cloud with much open sky. So: scattered cloud whenever the game draws its puffs (fading with
    // them), towards overcast only with rain. Eased over seconds: the game fades puffs in and out and moves them round
    // the camera - the layer mustn't pulse with them. (Only how cloudy and what kind: where the puffs are says nothing
    // about the world - they stay round the eye.)
    const float present = 1.0f - std::exp(-0.5f * cover);   // (the puffs drawn: ~1 for any sky the game has clouds in)
    // (With the game's sheets drawn behind (RVK_CloudSheet): the heaps sparser - the sheets the veil, as the game's.)
    // The sky's own weather (sky.cpp, RVK_SkyVary): clear .. overcast from the clock - the game's sheets fading with it
    // (none on a clear day: user), the layer's cloudiness following it; the game's rain overcast as before.
    const float vary = std::clamp(m_cloudParams.vary, 0.0f, 1.0f);
    const float sky = SkyCloudiness();
    const float rainy = smoothstep01(0.0f, 0.2f, rain);
    m_skySheets = std::max(1.0f + (smoothstep01(0.08f, 0.4f, sky) - 1.0f) * vary, rainy);
    const float sheets = CloudSheetsOn() ? std::min(m_cloudParams.sheet, 1.0f) * m_skySheets : 0.0f;
    const float fixedSky = 0.42f + 0.05f * flatShare - 0.09f * sheets;   // (as the game's puffs say: always scattered)
    const float ownSky = 0.95f * sky - 0.06f * sheets;   // (overcast: hardly a gap)
    // A thunderstorm (the rain: the game's, or RVK_StormForce): the sky closed over whatever else says - no open sky.
    const float sealed = smoothstep01(0.05f, 0.5f, rain);
    const float cloudy = std::max(std::clamp(present * (fixedSky + (ownSky - fixedSky) * vary) + rainy * 0.55f, 0.0f, 1.0f), sealed);
    // (Flat sheets: lumpy layered cloud - stratocumulus - not a thin stratus band, which seen from below at a low
    // angle smears into waves and streaks.)
    const float type = std::clamp(0.5f + 0.5f * rain - 0.12f * flatShare * (1.0f - rain) +
                                  0.25f * vary * smoothstep01(0.6f, 0.95f, sky), 0.0f, 1.0f);   // (broken, overcast: layered)
    {
        const double gap = m_cloudWeatherClock < 0.0 ? 1e9 : m_frameClock - m_cloudWeatherClock;
        const float k = gap > 5.0 ? 1.0f : 1.0f - std::exp(-float(gap) / 8.0f);
        m_cloudCloudy += (cloudy - m_cloudCloudy) * k;
        m_cloudType += (type - m_cloudType) * k;
        m_cloudWispy += ((weight > 0.0f ? wispy / weight : 0.0f) - m_cloudWispy) * k;   // (the game's wispy puffs' share)
        // The low deck (stratus): with rain, under the game's flat sheets. (Not by how heavily the game fogs its clouds:
        // its storms 73-77%, but a fair day in a teal-fogged zone 79%.)
        // (In a thunderstorm whatever the game draws: ragged low cloud under the storm's base.)
        const float deck = std::max(std::clamp(smoothstep01(0.4f, 1.0f, flatShare) * smoothstep01(0.03f, 0.2f, rain), 0.0f, 1.0f),
                                    0.85f * smoothstep01(0.1f, 0.6f, rain));
        m_cloudDeck += (deck - m_cloudDeck) * k;
        m_cloudWeatherClock = m_frameClock;
    }
    wf.info[1] = m_cloudCloudy;
    wf.storm[0] = smoothstep01(0.1f, 0.7f, rain);   // (cloud_weather.frag: the storm's cover sealed)
    wf.storm[1] = kStormReach;                       // ... out to the horizon: one mass (user - no towers apart)
    wf.info[2] = m_cloudType;
    static const float typeOverride = getenv("RANDYVK_CLOUD_TYPE") ? float(atof(getenv("RANDYVK_CLOUD_TYPE"))) : -1.0f;
    if (typeOverride >= 0.0f)
        wf.info[2] = typeOverride;               // (RANDYVK_CLOUD_TYPE: tests of a cloud type, 0 stratus .. 1 storm)
    wf.info[3] = 1.0f;
    wf.drift[0] = m_cloudDrift[0];             // (UpdateCloudDrift)
    wf.drift[1] = m_cloudDrift[1];
    wf.drift[2] = 9000.0f;
    wf.drift[3] = std::clamp(m_cloudParams.cover, -1.0f, 1.0f);
    if (m_frameNumber % 600 == 1)
        Log("clouds: sky %s (%.2f, RVK_SkyVary %.2f; the game's sheets x%.2f; wind %.1f towards %.2f %.2f); weather map: cloudy %.2f (now %.2f: puffs "
            "%.2f, rain %.2f), type %.2f (flat sheets %.0f%%), base %.0f (eye %.0f); low deck %.2f; wispy puffs %.0f%% "
            "(%u of %zu puffs' textures known)", SkyName(sky), double(sky), double(vary), double(m_skySheets),
            double(m_cloudWindSpeed), double(m_cloudWindDir[0]), double(m_cloudWindDir[1]),
            double(wf.info[1]), double(cloudy), double(cover), double(rain), double(wf.info[2]), double(flatShare * 100.0f),
            double(m_cloudBase), double(eye[1]), double(m_cloudDeck), double(m_cloudWispy * 100.0f), known,
            m_cloudPuffs.size());
    m_cloudWeatherArea[0] = wf.area[0];
    m_cloudWeatherArea[1] = wf.area[1];
    m_cloudWeatherArea[2] = area;
    Frame& f = m_frames[m_frameIndex];
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(WeatherFrame), 256, &cpu);
    std::memcpy(cpu, &wf, sizeof(wf));
    Transition(cmd, m_cloudWeather, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_cloudWeather->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;   // (cleared: the pipeline blends over - its alpha is the base)
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kWeatherSize, kWeatherSize}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    VkViewport viewport{0.0f, 0.0f, float(kWeatherSize), float(kWeatherSize), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {kWeatherSize, kWeatherSize}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudWeatherPipeline);
    VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(WeatherFrame)};
    // (The layer's shape noise, 128^3: smooth over many texels - v1's 64^3 has its finest cells a few texels wide, and
    // its texel grid showed through as blocks, extruded through the layer as streaks.)
    VkDescriptorImageInfo noiseInfo{m_cloudRepeatSampler, m_cloudShape.view, VK_IMAGE_LAYOUT_GENERAL};
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
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudWeatherLayout, 0, 2, w);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudWeather, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
}

// RVK_CloudLayer: the layer marched over the half-size target (inside its rendering), one fullscreen pass. The world
// camera: the frame's (the first scene draw writing depth), else the draw's that set the clouds off (the first after
// the sky).
void Device::MarchCloudLayer(VkCommandBuffer cmd, bool skipCovered, const float sub[4], const float* shadowArea,
                             bool sunRay)
{
    Frame& f = m_frames[m_frameIndex];
    const uint32_t hw = (m_scene->m_width + 1) / 2, hh = (m_scene->m_height + 1) / 2;
    uint32_t tw = sub[2] > 1.5f ? (hw + 1) / 2 : hw, th = sub[2] > 1.5f ? (hh + 1) / 2 : hh;   // the target's
    if (shadowArea)
        tw = th = kShadowSize;                   // (the shadow map: cloud_layer_shadow.frag)
    if (sunRay)
        tw = kSunRays, th = 1;                   // (the rays to the sun: cloud_layer_sunray.frag)
    const d3d::Matrix& view = m_aoProjValid ? m_aoView : m_view;
    const d3d::Matrix& proj = m_aoProjValid ? m_aoProj : m_proj;
    LayerFrame lf{};
    if (!InvertMatrix(MulMatrix(view, proj), &lf.invViewProj))
        return;
    const auto& vm = view.m;
    for (int i = 0; i < 3; ++i)
        lf.eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    const double clock = std::fmod(m_frameClock, 3600.0);
    lf.eye[3] = float(clock);
    lf.layer[0] = m_cloudBase;
    // (A thunderstorm's layer taller: its towers - cloud_layer.frag Profile - the rest keep to the lower 1300.)
    lf.layer[1] = m_cloudBase + kLayerThickness * (1.0f + 2.0f * smoothstep01(0.1f, 0.7f, StormSky()));
    lf.layer[2] = kLayerRadius;
    lf.layer[3] = kLayerReach + (kStormReach - kLayerReach) * smoothstep01(0.1f, 0.7f, StormSky());
    lf.weather[0] = m_cloudWeatherArea[0];
    lf.weather[1] = m_cloudWeatherArea[1];
    lf.weather[2] = 1.0f / m_cloudWeatherArea[2];
    lf.weather[3] = skipCovered ? 1.0f : 0.0f;
    lf.shape[0] = 1.0f / 4500.0f;                // (4 cells a tile: cloud cells about a kilometre)
    lf.shape[1] = 1.0f / 900.0f;                 // (a tile of 4 .. 16 detail cells: ~225 .. 56 units each)
    // Detail and density by the weather (RVK_CloudDetail / RVK_CloudThick scale them): storms smooth, heavy masses -
    // 30% less erosion at full rain, 20% denser; clearer skies crisper, more ragged edges (up to +20%). (Eased: rain
    // and cloudiness are.)
    const float stormNow = StormSky();
    const float weatherDetail = (1.0f - 0.3f * stormNow) * (1.0f + 0.2f * (1.0f - std::clamp(m_cloudCloudy, 0.0f, 1.0f)));
    lf.shape[2] = 0.65f * m_cloudParams.detail * weatherDetail;
    lf.shape[3] = 0.02f * m_cloudParams.density * (1.0f + 0.6f * stormNow);   // (a storm's: heavy, dark underneath)
    // The wind carries the shapes (and they evolve: up through the noise) - UpdateCloudDrift.
    lf.wind[0] = m_cloudDrift[0];
    lf.wind[1] = m_cloudDrift[2];
    lf.wind[2] = m_cloudDrift[1];
    lf.wind[3] = m_prepassClearZ;
    // The game's cloud colour as it shows them: each puff lit as it lit it, fogged as it fogs it (its fog by its
    // distance - storms draw their far puffs wholly in the fog's colour: the green storm sky), averaged by opacity.
    // The sky's light and the distance's haze: the fog the game draws its clouds in (not the ground's). Eased over
    // seconds (puffs fade in and out).
    float tint[3] = {0.0f, 0.0f, 0.0f}, fogColour[3] = {0.0f, 0.0f, 0.0f}, weight = 0.0f, fogWeight = 0.0f, fogged = 0.0f;
    for (const CloudPuff& p : m_cloudPuffs) {
        const float o = std::clamp(p.colour[3], 0.05f, 1.0f);
        float f = 1.0f;                           // (1: no fog)
        if (p.fogColour[3] > 0.5f) {
            float mid[3], w[3];
            for (int k = 0; k < 3; ++k) mid[k] = 0.5f * (p.low[k] + p.high[k]);
            for (int j = 0; j < 3; ++j)
                w[j] = mid[0] * p.world.m[0][j] + mid[1] * p.world.m[1][j] + mid[2] * p.world.m[2][j] + p.world.m[3][j];
            const float z = std::fabs(w[0] * p.view.m[0][2] + w[1] * p.view.m[1][2] + w[2] * p.view.m[2][2] + p.view.m[3][2]);
            const uint32_t mode = uint32_t(p.fogParams[3]);
            if (mode == 3) f = std::clamp((p.fogParams[1] - z) / std::max(p.fogParams[1] - p.fogParams[0], 1e-3f), 0.0f, 1.0f);
            else if (mode == 1) f = std::exp(-p.fogParams[2] * z);
            else if (mode == 2) f = std::exp(-(p.fogParams[2] * z) * (p.fogParams[2] * z));
            for (int k = 0; k < 3; ++k) fogColour[k] += p.fogColour[k] * o;
            fogWeight += o;
        }
        for (int k = 0; k < 3; ++k)
            tint[k] += (p.fogColour[3] > 0.5f ? p.fogColour[k] + (p.colour[k] - p.fogColour[k]) * f : p.colour[k]) * o;
        fogged += (1.0f - f) * o;
        weight += o;
    }
    float sceneFog[4], sceneFogParams[4];
    SceneFog(sceneFog, sceneFogParams);
    float tintNow[3], hazeNow[3];
    for (int k = 0; k < 3; ++k) {
        tintNow[k] = weight > 0.0f ? tint[k] / weight : 0.8f;
        hazeNow[k] = fogWeight > 0.0f ? fogColour[k] / fogWeight
                   : sceneFog[3] > 0.5f ? sceneFog[k] : (m_terrainAmbient[3] > 0.5f ? m_terrainAmbient[k] : 0.35f);
    }
    StormHue(tintNow);                           // (a thunderstorm: the game's puffs and its fog dark yellow-green)
    StormHue(hazeNow);
    const float foggedNow = weight > 0.0f ? fogged / weight : 0.0f;
    {
        const double gap = m_cloudColourClock < 0.0 ? 1e9 : m_frameClock - m_cloudColourClock;
        const float k = gap > 5.0 ? 1.0f : 1.0f - std::exp(-float(gap) / 3.0f);
        for (int i = 0; i < 3; ++i) {
            m_cloudTint[i] += (tintNow[i] - m_cloudTint[i]) * k;
            m_cloudHaze[i] += (hazeNow[i] - m_cloudHaze[i]) * k;
        }
        m_cloudFogged += (foggedNow - m_cloudFogged) * k;
        m_cloudColourClock = m_frameClock;
    }
    // Towards the horizon the clouds become the fog's colour as far as the game's are fogged (over half at least: the
    // far distance is hazy, as the game's sky).
    // (A thunderstorm: its clouds pronounced - less of the horizon's haze over them, more light and shade - the user.)
    const float stormLook = StormSky();
    lf.horizon[0] = std::clamp(0.55f + 0.45f * m_cloudFogged, 0.0f, 1.0f) * (1.0f - 0.4f * stormLook);
    lf.horizon[1] = std::clamp(m_cloudParams.shade * (1.0f + 0.5f * stormLook), 0.25f, 4.0f);
    // The game's colour for its clouds, mostly whitened: its brightness kept, a little of its hue (the zones' teal
    // skies made them very blue - user: whiter).
    {
        // (And no darker than 0.62 by day: the game's fogged puffs are grey - ours white by day, dimmer at night.)
        const float l = 0.3f * m_cloudTint[0] + 0.59f * m_cloudTint[1] + 0.11f * m_cloudTint[2];
        const float lift = l > 1e-3f ? std::max(1.0f, 0.62f * std::clamp(m_daylight / 0.35f, 0.0f, 1.0f) / l) : 1.0f;
        for (int k = 0; k < 3; ++k) lf.tint[k] = (l + (m_cloudTint[k] - l) * 0.3f) * lift;
    }
    const float storm = StormSky();
    {
        // RVK_CloudNight: by night (as the shaders' Night(): by the sun's true height) their brightness x more.
        const float l = std::max(std::sqrt(m_trueSunDir[0] * m_trueSunDir[0] + m_trueSunDir[1] * m_trueSunDir[1] +
                                           m_trueSunDir[2] * m_trueSunDir[2]), 1e-6f);
        const float t = std::clamp((-m_trueSunDir[1] / l + 0.2f) / 0.23f, 0.0f, 1.0f);
        const float night = 1.0f - t * t * (3.0f - 2.0f * t);
        lf.tint[3] = m_cloudParams.brightness * (1.0f + (m_cloudParams.night - 1.0f) * night) * (1.0f - 0.5f * storm);
    }
    // The sky's light: its hue (the haze's, a quarter: whiter clouds), about unit brightness - the tint carries how light.
    const float luma = std::max(0.3f * m_cloudHaze[0] + 0.59f * m_cloudHaze[1] + 0.11f * m_cloudHaze[2], 1e-3f);
    for (int k = 0; k < 3; ++k) lf.ambient[k] = 0.75f + 0.25f * m_cloudHaze[k] / luma;
    lf.ambient[3] = storm;
    {   // (A thunderstorm's green cast - RVK_StormGreen: the layer's colour and the sky's light on it.)
        // (... and darker: the user - a dark yellow-green storm. Its hue: the game's colours, StormHue'd.)
        const float dark = 1.0f - 0.35f * std::clamp(m_stormGreen, 0.0f, 1.0f) * storm;
        StormHue(lf.tint);
        StormHue(lf.ambient);
        for (int k = 0; k < 3; ++k) lf.tint[k] *= dark;
    }
    for (int k = 0; k < 3; ++k) lf.haze[k] = m_cloudHaze[k];
    if (m_frameNumber % 600 == 2)
        Log("clouds: colour %.2f %.2f %.2f (the puffs as the game shows them: %.0f%% fog), haze %.2f %.2f %.2f",
            double(m_cloudTint[0]), double(m_cloudTint[1]), double(m_cloudTint[2]),
            double(weight > 0.0f ? 100.0f * fogged / weight : 0.0f), double(m_cloudHaze[0]),
            double(m_cloudHaze[1]), double(m_cloudHaze[2]));
    lf.haze[3] = 18000.0f;
    // The game's sheets (next frame's draws: FrameLights) take the same colours.
    std::memcpy(m_sheetTint, lf.tint, sizeof(m_sheetTint));
    std::memcpy(m_sheetSky, lf.ambient, sizeof(m_sheetSky));
    for (int k = 0; k < 3; ++k) m_sheetAir[k] = lf.haze[k];
    m_sheetAir[3] = lf.horizon[0];
    lf.view[0] = float(hw);
    lf.view[1] = float(hh);
    lf.view[2] = float(std::clamp(m_cloudParams.steps * 3u, 32u, 192u));
    // RVK_CloudQual: the march's steps shorter (less noise - its offsets averaged over frames cover a finer step), more
    // steps towards the sun.
    static const float kStepScale[4] = {1.4f, 1.0f, 0.7f, 0.5f}, kSunSteps[4] = {3.0f, 4.0f, 5.0f, 6.0f};
    const uint32_t quality = std::min(m_cloudParams.quality, 3u);
    lf.view[3] = kSunSteps[quality];
    lf.quality[0] = kStepScale[quality];
    std::memcpy(lf.sub, sub, sizeof(lf.sub));
    if (shadowArea)
        std::memcpy(lf.shadowArea, shadowArea, sizeof(lf.shadowArea));
    static const float deckOverride = getenv("RANDYVK_CLOUD_DECK") ? float(atof(getenv("RANDYVK_CLOUD_DECK"))) : -1.0f;
    lf.deck[0] = std::clamp((deckOverride >= 0.0f ? deckOverride : m_cloudDeck) * m_cloudParams.low, 0.0f, 1.0f);
    // (Between the ground and the layer's base; a thunderstorm's lower - its scud, near over the land.)
    const float scud = smoothstep01(0.1f, 0.7f, StormSky());
    lf.deck[1] = m_cloudBase - (0.55f + 0.15f * scud) * m_cloudParams.height;
    lf.deck[2] = lf.deck[1] + kDeckThickness;
    // The game's sheets' faint shadow (the shadow map), while it draws them.
    lf.deck[3] = CloudSheetsOn() && !m_cloudPuffs.empty() ? 0.25f * std::min(m_cloudParams.sheet, 1.0f) * m_skySheets
                                                          : 0.0f;
    lf.scud[0] = m_scudDrift[0];
    lf.scud[1] = m_scudDrift[1];
    lf.scud[2] = scud;                           // (cloud_layer.frag DeckDensity: torn into ragged fragments)
    // The churn (UpdateCloudDrift): its field's phase, how far it pushes (body tiles: ~180 units fair, ~450 a storm's -
    // the edges and lumps changing, not whole clouds bulging: user, a lava lamp at 3x), the detail's extra rise.
    {
        const float morph = std::clamp(m_cloudParams.morph, 0.0f, 3.0f);
        lf.morph[0] = m_cloudMorphPhase;
        lf.morph[1] = std::min(morph, 1.0f) * (0.04f + 0.06f * StormSky());
        lf.morph[2] = m_cloudBoil;
        // (The rain veils under a storm: some with its sky alone, all while it rains - any rain: the game's is ~0.1.)
        const float rainy = smoothstep01(0.0f, 0.05f, std::clamp(m_rain, 0.0f, 1.0f));
        lf.morph[3] = m_weatherParams.enable ? StormSky() * (0.3f + 0.7f * rainy) * std::clamp(m_rainVeils, 0.0f, 3.0f) : 0.0f;
    }
    void* cpu = nullptr;
    if (!shadowArea && !sunRay) {                // (the view's march: what its lightning needs - DrawClouds)
        m_cloudUpInvViewProj = lf.invViewProj;
        for (int i = 0; i < 3; ++i) m_cloudUpEye[i] = lf.eye[i];
        m_cloudUpEye[3] = lf.layer[0];
        for (int i = 0; i < 3; ++i) m_cloudUpTint[i] = lf.tint[i];
        m_cloudUpTint[3] = lf.haze[3];
        m_cloudUpReach = lf.layer[3];
        m_cloudUpFrameNumber = m_frameNumber;
    }
    const VkDeviceSize offset = Allocate(sizeof(LayerFrame), 256, &cpu);
    std::memcpy(cpu, &lf, sizeof(lf));
    VkViewport viewport{0.0f, 0.0f, float(tw), float(th), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {tw, th}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                      sunRay ? m_cloudSunRayPipeline : shadowArea ? m_cloudLayerShadowPipeline : m_cloudLayerPipeline);
    VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(LayerFrame)};
    VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
    VkDescriptorImageInfo weatherInfo{m_cloudClampSampler, m_cloudWeather->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo shapeInfo{m_cloudRepeatSampler, m_cloudShape.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo detailInfo{m_cloudRepeatSampler, m_cloudDetail.view, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthInfo{m_pointSampler, skipCovered ? m_depthView : m_blackTexture->m_view,
                                    VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo blueInfo{m_pointSampler, m_blueNoise->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[7] = {};
    const VkDescriptorImageInfo* images[7] = {nullptr, &weatherInfo, &shapeInfo, &depthInfo, nullptr, &detailInfo, &blueInfo};
    for (uint32_t i = 0; i < 7; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = images[i] ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[i].pImageInfo = images[i];
    }
    w[0].pBufferInfo = &blockInfo;
    w[4].pBufferInfo = &lightsInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudLayerLayout, 0, 7, w);
    vkCmdDraw(cmd, 3, 1, 0, 0);
}

// RVK_CloudLayer reused over frames: this frame marches one half-size pixel of each 2x2 (taking turns over four
// frames) into the quarter-size target; cloud_resolve.frag rebuilds the half-size clouds from them and last frame's,
// turned with the camera. Last frame's are dropped when there were none (a frame without clouds, a resize) or the
// eye jumped (a teleport, a zone). Returns the half-size clouds (premultiplied).
Texture* Device::ReusedCloudLayer(VkCommandBuffer cmd, bool skipCovered, const float eye[3])
{
    Frame& f = m_frames[m_frameIndex];
    const uint32_t hw = (m_scene->m_width + 1) / 2, hh = (m_scene->m_height + 1) / 2;
    const uint32_t qw = (hw + 1) / 2, qh = (hh + 1) / 2;
    auto ensure = [&](Texture*& t, uint32_t w, uint32_t h) {
        if (t && t->m_width == w && t->m_height == h)
            return;
        if (t) DestroyTexture(t);
        t = CreateImage(w, h, Format::RGBA16F, 1, true);
        m_cloudHistoryValid = false;
    };
    ensure(m_cloudQuarter, qw, qh);
    ensure(m_cloudHistory[0], hw, hh);
    ensure(m_cloudHistory[1], hw, hh);
    if (!m_cloudQuarter || !m_cloudHistory[0] || !m_cloudHistory[1] || !m_cloudResolvePipeline)
        return m_cloudHalf;
    const float jump = std::sqrt((eye[0] - m_cloudHistoryEye[0]) * (eye[0] - m_cloudHistoryEye[0]) +
                                 (eye[1] - m_cloudHistoryEye[1]) * (eye[1] - m_cloudHistoryEye[1]) +
                                 (eye[2] - m_cloudHistoryEye[2]) * (eye[2] - m_cloudHistoryEye[2]));
    const bool valid = m_cloudHistoryValid && m_cloudHistoryFrame + 1 == m_frameNumber && jump < 100.0f;
    // This frame's pixel of each 2x2 (all four over four frames, diagonals apart).
    static const float kOrder[4][2] = {{0, 0}, {1, 1}, {1, 0}, {0, 1}};
    const float* o = kOrder[m_frameNumber % 4];
    const float sub[4] = {o[0], o[1], 2.0f, 0.0f};
    auto begin = [&](Texture* t, uint32_t w, uint32_t h) {
        Transition(cmd, t, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
        VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
        color.imageView = t->m_view;
        color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {w, h}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = 1;
        ri.pColorAttachments = &color;
        vkCmdBeginRendering(cmd, &ri);
    };
    begin(m_cloudQuarter, qw, qh);
    MarchCloudLayer(cmd, skipCovered, sub);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudQuarter, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    // The rebuild.
    const d3d::Matrix& view = m_aoProjValid ? m_aoView : m_view;
    const d3d::Matrix& proj = m_aoProjValid ? m_aoProj : m_proj;
    const d3d::Matrix rotation = RotationViewProj(view, proj);
    ResolveFrame rf{};
    if (!InvertMatrix(rotation, &rf.invViewProj))
        return m_cloudHalf;
    rf.prevViewProj = m_cloudPrevRotation;
    rf.size[0] = float(hw);
    rf.size[1] = float(hh);
    rf.size[2] = float(qw);
    rf.size[3] = float(qh);
    rf.sub[0] = o[0];
    rf.sub[1] = o[1];
    rf.sub[2] = valid ? 1.0f : 0.0f;
    // (A still view: more of the history - each pixel's march offset averaged over more frames, smoother edges; turning,
    // less, so nothing trails.)
    {
        float turn = 0.0f;
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j) turn += std::fabs(rotation.m[i][j] - m_cloudPrevRotation.m[i][j]);
        rf.sub[3] = turn < 0.003f ? 0.2f : 0.4f;
    }
    const uint32_t cur = m_cloudHistoryIndex ^ 1u, prev = m_cloudHistoryIndex;
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(ResolveFrame), 256, &cpu);
    std::memcpy(cpu, &rf, sizeof(rf));
    begin(m_cloudHistory[cur], hw, hh);
    VkViewport viewport{0.0f, 0.0f, float(hw), float(hh), 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {hw, hh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudResolvePipeline);
    VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(ResolveFrame)};
    VkDescriptorImageInfo quarterInfo{m_cloudClampSampler, m_cloudQuarter->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    // (Last frame's only when it holds clouds - a target never drawn into has no layout to read.)
    VkDescriptorImageInfo historyInfo{m_cloudClampSampler, valid ? m_cloudHistory[prev]->m_view : m_blackTexture->m_view,
                                      VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[3] = {};
    for (uint32_t i = 0; i < 3; ++i) {
        w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        w[i].dstBinding = i;
        w[i].descriptorCount = 1;
        w[i].descriptorType = i == 0 ? VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER : VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    }
    w[0].pBufferInfo = &blockInfo;
    w[1].pImageInfo = &quarterInfo;
    w[2].pImageInfo = &historyInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_cloudResolveLayout, 0, 3, w);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudHistory[cur], VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    m_cloudPrevRotation = rotation;
    m_cloudHistoryIndex = cur;
    m_cloudHistoryValid = true;
    m_cloudHistoryFrame = m_frameNumber;
    std::memcpy(m_cloudHistoryEye, eye, sizeof(m_cloudHistoryEye));
    return m_cloudHistory[cur];
}

// RVK_CloudLayer's shadow map: the sunlight through the layer, top down in a plane through it (cloud_layer_shadow.frag,
// the weather map made already) - the ground's shadows match the clouds seen. Placed where the sun's rays through
// the ground round the camera cross the plane. Outside any rendering.
void Device::MakeCloudLayerShadow(VkCommandBuffer cmd, const float eye[3])
{
    if (!m_cloudShadowMap) {
        m_cloudShadowMap = CreateImage(kShadowSize, kShadowSize, Format::RGBA8, 1, true);
        if (!m_cloudShadowMap)
            return;
    }
    float sun[3] = {-m_frameSunDir[0], -m_frameSunDir[1], -m_frameSunDir[2]};
    const float sl = std::max(std::sqrt(sun[0] * sun[0] + sun[1] * sun[1] + sun[2] * sun[2]), 1e-6f);
    for (float& v : sun) v /= sl;
    const float plane = m_cloudBase + 0.35f * kLayerThickness;
    const float up = std::max(sun[1], 0.2f);    // (low sun: the offset capped)
    const float cx = eye[0] + (plane - eye[1]) * sun[0] / up, cz = eye[2] + (plane - eye[1]) * sun[2] / up;
    const float area = kLayerShadowArea, texel = area / float(kShadowSize);
    float shadowArea[4] = {std::floor((cx - area * 0.5f) / texel) * texel, std::floor((cz - area * 0.5f) / texel) * texel,
                           area, plane};
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
    // (w: how cloudy the sky is - the shadow's fine patches in the open: cloud_layer.frag RVK_CLOUD_SHADOW.)
    const float sub[4] = {0.0f, 0.0f, 1.0f, std::clamp(m_cloudCloudy + 0.5f * m_cloudParams.cover, 0.0f, 1.0f)};
    MarchCloudLayer(cmd, false, sub, shadowArea);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudShadowMap, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
    std::memcpy(m_cloudShadowArea, shadowArea, sizeof(m_cloudShadowArea));
    m_cloudShadowFrame = m_frameNumber;
}

// How much sunlight the clouds let through to the eye (cloud_layer_sunray.frag: kSunRays rays towards the sun through
// the layer and the deck), copied for the CPU (sun.cpp DrawSun reads it when this frame slot comes round again): low,
// the frame's sun is dimmed by it - in the world, whatever the camera sees (RenderShadowMap).
void Device::MakeCloudSunRay(VkCommandBuffer cmd)
{
    m_sunRayMeasured[m_frameIndex] = false;
    if (!m_cloudSunRayPipeline || !m_sunVisBuffer)
        return;
    if (!m_cloudSunRay) {
        m_cloudSunRay = CreateImage(kSunRays, 1, Format::RGBA8, 1, true);
        if (!m_cloudSunRay)
            return;
    }
    Transition(cmd, m_cloudSunRay, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
    VkRenderingAttachmentInfo color{VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO};
    color.imageView = m_cloudSunRay->m_view;
    color.imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
    ri.renderArea = {{0, 0}, {kSunRays, 1}};
    ri.layerCount = 1;
    ri.colorAttachmentCount = 1;
    ri.pColorAttachments = &color;
    vkCmdBeginRendering(cmd, &ri);
    const float sub[4] = {0.0f, 0.0f, 1.0f, 0.0f};
    MarchCloudLayer(cmd, false, sub, nullptr, true);
    vkCmdEndRendering(cmd);
    Transition(cmd, m_cloudSunRay, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL);
    VkBufferImageCopy region{};
    region.bufferOffset = m_sunVisStride * m_frameIndex + kSunRayOffset;
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {kSunRays, 1, 1};
    vkCmdCopyImageToBuffer(cmd, m_cloudSunRay->m_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_sunVisBuffer, 1, &region);
    VkMemoryBarrier2 mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER_2};
    mb.srcStageMask = VK_PIPELINE_STAGE_2_TRANSFER_BIT;
    mb.srcAccessMask = VK_ACCESS_2_TRANSFER_WRITE_BIT;
    mb.dstStageMask = VK_PIPELINE_STAGE_2_HOST_BIT;
    mb.dstAccessMask = VK_ACCESS_2_HOST_READ_BIT;
    VkDependencyInfo dep{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
    dep.memoryBarrierCount = 1;
    dep.pMemoryBarriers = &mb;
    vkCmdPipelineBarrier2(cmd, &dep);
    m_sunRayMeasured[m_frameIndex] = true;
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
    // RVK_CloudLayer's: the shape (128^3) and the detail (32^3).
    if (m_cloudLayerNoisePipeline && !m_cloudLayerNoiseMade) {
        m_cloudLayerNoiseMade = true;
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cloudLayerNoisePipeline);
        for (uint32_t mode = 0; mode < 2; ++mode) {
            const CloudVolume& v = mode == 0 ? m_cloudShape : m_cloudDetail;
            const uint32_t size = mode == 0 ? kShapeSize : kDetailSize;
            VkImageMemoryBarrier2 vb = b;
            vb.image = v.image;
            dep = VkDependencyInfo{VK_STRUCTURE_TYPE_DEPENDENCY_INFO};
            dep.imageMemoryBarrierCount = 1;
            dep.pImageMemoryBarriers = &vb;
            vkCmdPipelineBarrier2(cmd, &dep);
            VkDescriptorImageInfo vi{VK_NULL_HANDLE, v.view, VK_IMAGE_LAYOUT_GENERAL};
            VkWriteDescriptorSet vw = w;
            vw.pImageInfo = &vi;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_cloudLayerNoiseLayout, 0, 1, &vw);
            const uint32_t push[2] = {mode, size};
            vkCmdPushConstants(cmd, m_cloudLayerNoiseLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(push), push);
            vkCmdDispatch(cmd, size / 4, size / 4, size / 4);
        }
    }
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
