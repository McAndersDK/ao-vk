// Enhancement: the weather (RVK_WeatherOn). The game's precipitation - VisualRain_t (DisplaySystem): a few hundred
// lines in a 15-unit box round the camera, moved each frame by VisualRain_t::process - is hidden by the proxy, which
// hands over what the game asked for instead (SetWeather: how hard, the wind, the drops' colour, fall speed and
// opacity). rvk draws its own rain from that, after the scene (EndScene): streaks in a box round the camera, each
// drop's place a function of its index and the time alone (rain.vert), lit by the sun and the lamps near.
//
// The game's rain weather (Gamecode) calls process with its intensity cubed x 0.45 and the drops' opacity x 0.7;
// another weather object (the storms) passes its intensity as it is. Either way the cube root of value / 0.45 is the
// weather's own 0..1 (clamped).
#include "internal.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <string>

namespace rvk {

using namespace vk;
using namespace detail;

namespace {

const uint32_t kRainVertSpirv[] = {
#include "rain.vert.inc"
};
const uint32_t kRainFragSpirv[] = {
#include "rain.frag.inc"
};

// The per-draw block (rain.vert / rain.frag RainFrame, std140).
struct RainFrame {
    d3d::Matrix viewProj, prevViewProj, view;
    float camera[4];                             // xyz: the eye; w: 1 = the TAA's jitter
    float time[4];                               // x: the rain's clock (s); y: last frame's; z: how hard (0..1); w: drops
    float fall[4];                               // xyz: the drops' velocity (world, units / s); w: the box's half size
    float colour[4];                             // rgb: the drops' tint (the game's); w: their opacity
    float ambient[4];                            // rgb: the terrain's ambient light
    float fogColour[4];                          // rgb; w: 1 = fog on
    float fogParams[4];                          // start, end, density, table mode
    float viewport[4];                           // width, height, 1 / width, 1 / height
};

constexpr uint32_t kRainMaxDrops = 24000;       // at full rain, RVK_RainAmount 1
constexpr float kRainBox = 16.0f;               // half size of the box round the camera (units)
constexpr float kRainSpeed = 9.0f;              // a raindrop's fall (units / s; about a metre)
constexpr float kGameRainMax = 0.45f;           // the game's rain weather: process(intensity^3 x this)

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

void Device::SetWeather(const WeatherState& state)
{
    m_weatherGame = state;
    m_weatherGameTime = m_frameClock;
}

bool Device::CreateRainResources(std::string* error)
{
    DestroyRainResources();
    // One pushed set: 0 the block, 4 the frame lights.
    VkDescriptorSetLayoutBinding b[2] = {};
    b[0].binding = 0;
    b[1].binding = 4;
    for (auto& x : b) {
        x.descriptorCount = 1;
        x.descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        x.stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
    sl.bindingCount = 2;
    sl.pBindings = b;
    if (!Check(vkCreateDescriptorSetLayout(m_device, &sl, nullptr, &m_rainSetLayout), "vkCreateDescriptorSetLayout", error))
        return false;
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &m_rainSetLayout;
    if (!Check(vkCreatePipelineLayout(m_device, &pl, nullptr, &m_rainLayout), "vkCreatePipelineLayout", error))
        return false;
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "vkCreateShaderModule", error);
    };
    VkShaderModule vert = VK_NULL_HANDLE, frag = VK_NULL_HANDLE;
    bool ok = module(kRainVertSpirv, sizeof(kRainVertSpirv), &vert) && module(kRainFragSpirv, sizeof(kRainFragSpirv), &frag);
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
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        dss.depthTestEnable = VK_TRUE;
        dss.depthWriteEnable = VK_FALSE;
        dss.depthCompareOp = VK_COMPARE_OP_LESS_OR_EQUAL;
        // Premultiplied over every target alike (no independent blending): the shader gives the scene's colour its
        // light and the motion target the drops' motion (premultiplied too), the others nothing.
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
        ci.layout = m_rainLayout;
        ok = Check(vkCreateGraphicsPipelines(m_device, m_pipelineCache, 1, &ci, nullptr, &m_rainPipeline),
                   "vkCreateGraphicsPipelines (rain)", error);
    }
    if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
    if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
    return ok;
}

void Device::DestroyRainResources()
{
    if (m_rainPipeline) { vkDestroyPipeline(m_device, m_rainPipeline, nullptr); m_rainPipeline = VK_NULL_HANDLE; }
    if (m_rainLayout) { vkDestroyPipelineLayout(m_device, m_rainLayout, nullptr); m_rainLayout = VK_NULL_HANDLE; }
    if (m_rainSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_rainSetLayout, nullptr); m_rainSetLayout = VK_NULL_HANDLE; }
}

// The rain, over the finished scene (EndScene: its rendering just closed).
void Device::RenderRain(VkCommandBuffer cmd)
{
    const WeatherParams& P = m_weatherParams;
    // How hard it rains: forced, or the game's weather while it hands it over (over without a hand-off for half a
    // second), eased in and out over a few seconds as rain comes and goes.
    const double now = m_frameClock;
    const float dt = m_rainLastClock < 0.0 ? 0.0f : float(std::clamp(now - m_rainLastClock, 0.0, 0.25));
    m_rainLastClock = now;
    float target = 0.0f;
    if (P.enable) {
        if (P.force > 0.0f)
            target = std::min(P.force, 1.0f);
        else if (now - m_weatherGameTime < 0.5)
            target = std::clamp(std::cbrt(std::max(m_weatherGame.precipitation, 0.0f) / kGameRainMax), 0.0f, 1.0f);
    }
    m_rain += (target - m_rain) * (1.0f - std::exp(-dt / 2.5f));
    if (target == 0.0f && m_rain < 0.003f)
        m_rain = 0.0f;
    const bool fromGame = now - m_weatherGameTime < 0.5;
    if (fromGame && ++m_rainLogFrame % 600 == 1)
        Log("weather: the game's precipitation %.4f, wind %.2f %.2f %.2f, colour %06x, fall %.2f, wind scale %.3f, "
            "opacity %.2f -> rain %.2f (target %.2f)", double(m_weatherGame.precipitation), double(m_weatherGame.wind[0]),
            double(m_weatherGame.wind[1]), double(m_weatherGame.wind[2]), m_weatherGame.colour,
            double(m_weatherGame.fallSpeed), double(m_weatherGame.windScale), double(m_weatherGame.opacity),
            double(m_rain), double(target));
    m_rainClock += dt;
    if (m_rain <= 0.0f || !m_rainPipeline || !m_scene || !m_hdr)
        return;
    const uint32_t drops = std::min(uint32_t(float(kRainMaxDrops) * m_rain * std::clamp(P.amount, 0.0f, 4.0f)), 4u * kRainMaxDrops);
    if (!drops)
        return;

    // The wind leans the rain (the game's, smoothed; at most ~45 degrees).
    for (int i = 0; i < 3; ++i) {
        const float w = fromGame ? m_weatherGame.wind[i] : 0.0f;
        m_rainWind[i] += (w - m_rainWind[i]) * (1.0f - std::exp(-dt / 1.5f));
    }
    float wind[3] = {m_rainWind[0], 0.0f, m_rainWind[2]};
    const float wl = std::hypot(wind[0], wind[2]);
    if (wl > kRainSpeed) { wind[0] *= kRainSpeed / wl; wind[2] *= kRainSpeed / wl; }

    const auto& vm = m_view.m;
    float eye[3];
    for (int i = 0; i < 3; ++i)
        eye[i] = -(vm[3][0] * vm[i][0] + vm[3][1] * vm[i][1] + vm[3][2] * vm[i][2]);
    RainFrame rf{};
    rf.viewProj = MulMatrix(m_view, m_proj);
    rf.prevViewProj = m_prevViewProjValid ? MulMatrix(m_prevView, m_prevProj) : rf.viewProj;
    rf.view = m_view;
    rf.camera[0] = eye[0];
    rf.camera[1] = eye[1];
    rf.camera[2] = eye[2];
    rf.camera[3] = TaaActive() ? 1.0f : 0.0f;
    rf.time[0] = float(std::fmod(m_rainClock, 3600.0));
    rf.time[1] = float(std::fmod(m_rainClock - dt, 3600.0));
    rf.time[2] = m_rain;
    rf.time[3] = float(drops);
    rf.fall[0] = wind[0];
    rf.fall[1] = -kRainSpeed;
    rf.fall[2] = wind[2];
    rf.fall[3] = kRainBox;
    // The game's drop colour tints it (a colourless one: plain water); its opacity is the rain's own.
    const uint32_t c = fromGame && m_weatherGame.colour ? m_weatherGame.colour : 0xC8D0D8u;
    rf.colour[0] = float((c >> 16) & 0xFF) / 255.0f;
    rf.colour[1] = float((c >> 8) & 0xFF) / 255.0f;
    rf.colour[2] = float(c & 0xFF) / 255.0f;
    rf.colour[3] = 0.45f;
    if (m_terrainAmbient[3] > 0.5f)
        std::memcpy(rf.ambient, m_terrainAmbient, 16);
    else
        ArgbTo4(m_rs[d3d::RS_AMBIENT], rf.ambient);
    ArgbTo4(m_rs[d3d::RS_FOGCOLOR], rf.fogColour);
    rf.fogColour[3] = m_rs[d3d::RS_FOGENABLE] ? 1.0f : 0.0f;
    rf.fogParams[0] = AsFloatBits(m_rs[d3d::RS_FOGSTART]);
    rf.fogParams[1] = AsFloatBits(m_rs[d3d::RS_FOGEND]);
    rf.fogParams[2] = AsFloatBits(m_rs[d3d::RS_FOGDENSITY]);
    rf.fogParams[3] = float(m_rs[d3d::RS_FOGTABLEMODE] ? m_rs[d3d::RS_FOGTABLEMODE] : m_rs[d3d::RS_FOGVERTEXMODE]);
    rf.viewport[0] = float(m_scene->m_width);
    rf.viewport[1] = float(m_scene->m_height);
    rf.viewport[2] = 1.0f / rf.viewport[0];
    rf.viewport[3] = 1.0f / rf.viewport[1];

    EnsureRingSpace(sizeof(RainFrame) + sizeof(FrameLights) + 1024);
    if (m_frameLightsDirty || m_frameLightsGeneration != m_ringGeneration)
        WriteFrameLights();
    void* cpu = nullptr;
    const VkDeviceSize offset = Allocate(sizeof(RainFrame), 256, &cpu);
    std::memcpy(cpu, &rf, sizeof(rf));
    Frame& f = m_frames[m_frameIndex];

    BeginRenderingOn(m_scene);
    const float sw = float(m_scene->m_width), sh = float(m_scene->m_height);
    VkViewport viewport{0.5f, sh + 0.5f, sw, -sh, 0.0f, 1.0f};   // the scene draws into a flipped viewport
    VkRect2D scissor{{0, 0}, {m_scene->m_width, m_scene->m_height}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rainPipeline);
    VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(RainFrame)};
    VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
    VkWriteDescriptorSet writes[2] = {};
    for (int i = 0; i < 2; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    }
    writes[0].dstBinding = 0;
    writes[0].pBufferInfo = &blockInfo;
    writes[1].dstBinding = 4;
    writes[1].pBufferInfo = &lightsInfo;
    vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_rainLayout, 0, 2, writes);
    vkCmdDraw(cmd, drops * 6, 1, 0, 0);
    EndRendering();
    ProfileMark("rain");
}

}  // namespace rvk
