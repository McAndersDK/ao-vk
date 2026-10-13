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
// swapped each step); the curl, the divergence and the pressure are scratch, shared by all (RVK_SmokeExact: each
// volume keeps its pressure, the next solve starts from it). A volume steps about kSimStep apart whatever the frame
// rate; its drawing blends its last two states (the look image).
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

const uint32_t kSmokeSimSpirv[] = {
#include "smoke_sim.comp.inc"
};
const uint32_t kSmokeVertSpirv[] = {
#include "smoke.vert.inc"
};
const uint32_t kSmokeFragSpirv[] = {
#include "smoke.frag.inc"
};
const uint32_t kSmokeHalfFragSpirv[] = {        // smoke.frag built with RVK_SMOKE_HALF: into the half-size target
#include "smoke_half.frag.inc"
};
const uint32_t kFullscreenVertSpirv[] = {
#include "fullscreen.vert.inc"
};
const uint32_t kSmokeUpFragSpirv[] = {
#include "smoke_up.frag.inc"
};

constexpr uint32_t kSmokeMax = 8;               // volumes
constexpr uint32_t kSmokeSimMax = 6;            // ... simulated a frame (the nearest)
constexpr float kSmokeRange = 160.0f;           // simulated and drawn this near only
constexpr uint32_t kJacobi = 24;                // pressure steps, from 0 (even: the result back in the first image)
constexpr uint32_t kJacobiExact = 8;            // RVK_SmokeExact: ... from the last step's pressure, near this one's: so
                                                // closer to the converged pressure than 24 from 0 (its gradient's error
                                                // 0.24 against 0.72 in the demo; RANDYVK_SMOKE_RESIDUAL measures it)
constexpr float kWarmUp = 2.0f, kWarmSpeed = 3.0f;
constexpr float kSimStep = 1.0f / 60.0f;         // a volume steps about this often (s), whatever the frame rate - drawn
constexpr float kSimFar = 40.0f;                // between its last two states; beyond this (world) half as often
constexpr uint32_t kNoiseSize = 64;             // the wisps' noise image (cells a side)
constexpr float kFlowMax = 4.0f;                // the flow image's range (world / s; smoke_common.glsl kSmokeFlowMax)
constexpr float kExtinction = 3.2f;             // per world unit at density 1 (x RVK_SmokeDensity)

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
    float a[4], b[4], c[4], d[4], e[4], f[4], g[4], h[4];   // smoke_sim.comp Params
};
static_assert(sizeof(SimPush) == 128, "push constants");

// A box's cells a side: RVK_SmokeRes per world unit, a multiple of 4, at most RVK_SmokeQual's.
uint32_t SmokeCells(float size, const Device::SmokeParams& P)
{
    const uint32_t most = P.quality <= 1 ? 64u : P.quality == 2 ? 96u : 128u;
    const float want = std::ceil(size * std::max(P.resolution, 1.0f) / 4.0f) * 4.0f;
    return uint32_t(std::clamp(want, 32.0f, float(most)));
}

// Drift clouds: smoke a character carries out of a source's box goes on in a box of its own following them.
constexpr float kDriftSpeed = 0.8f;             // a character moving at least this fast (world / s) through a box
constexpr uint32_t kDriftPerSource = 2;         // ... gets one, at most this many a source
constexpr float kEdgeFade = 0.2f;               // the band along a box's walls and top its smoke leaves over (box
                                                // units; smoke.frag thins it out there, a drift cloud takes it)

// The half-size smoke's upsampling (smoke_up.frag).
struct SmokeUpPush {
    float proj[4];                              // D3D projection m[2][2], m[3][2]; the half-size target's width, height
};

constexpr uint32_t kLamps = 8;                  // lamps lighting a volume (those whose light reaches its box)
constexpr VkDeviceSize kBoundsSlot = 32;        // a volume's in m_smokeBounds: uvec4 lowest, uvec4 highest cell

// The per-draw block (smoke_common.glsl SmokeFrame, std140).
struct SmokeFrame {
    d3d::Matrix viewProj, view;
    float camera[4], box[4], colour[4], ambient[4], proj[4], viewport[4], fogColour[4], fogParams[4], detail[4], look[4];
    uint32_t info[4];                           // x: its slot in the bounds; y: lamps; z: cells a side
    uint32_t lamps[kLamps];                     // their indices in the frame lights
};

void ArgbTo4(uint32_t c, float out[4])
{
    out[0] = ((c >> 16) & 0xFF) / 255.0f;
    out[1] = ((c >> 8) & 0xFF) / 255.0f;
    out[2] = (c & 0xFF) / 255.0f;
    out[3] = (c >> 24) / 255.0f;
}

// Diagnostics (RANDYVK_SMOKE_RESIDUAL=F): at frame F the first volume's pressure and divergence are copied out; a few
// frames later the pressure equation's residual is logged (how well the solve converged).
struct SmokeResidual {
    VkBuffer buffer = VK_NULL_HANDLE;
    VmaAllocation_T* allocation = nullptr;
    void* cpu = nullptr;
    uint64_t frame = 0, readAt = 0;
    uint32_t n = 0;
    float h = 0.0f;
};
SmokeResidual g_residual;

// ... the solve's error, logged once the copy is done.
void LogSmokeResidual()
{
    g_residual.readAt = 0;
    const uint32_t n = g_residual.n;
    const float* pr = static_cast<const float*>(g_residual.cpu);
    const float* dv = pr + size_t(n) * n * n;
    // The reference: the same equation (6p - neighbours = -div h^2; the floor's neighbour below is the cell itself,
    // beyond the walls and the top 0) solved to convergence by conjugate gradients; the solve's error measured in
    // what it does - the pressure gradient taken from the velocity (central differences, as stage 5).
    const size_t N = size_t(n) * n * n;
    const float h2 = g_residual.h * g_residual.h;
    auto idx = [&](int x, int y, int z) { return (size_t(z) * n + y) * n + x; };
    auto apply = [&](const std::vector<double>& x, std::vector<double>& out) {
        for (int z = 0; z < int(n); ++z)
            for (int y = 0; y < int(n); ++y)
                for (int xx = 0; xx < int(n); ++xx) {
                    const double self = x[idx(xx, y, z)];
                    double sum = 0.0;
                    if (xx > 0) sum += x[idx(xx - 1, y, z)];
                    if (xx + 1 < int(n)) sum += x[idx(xx + 1, y, z)];
                    sum += y > 0 ? x[idx(xx, y - 1, z)] : self;
                    if (y + 1 < int(n)) sum += x[idx(xx, y + 1, z)];
                    if (z > 0) sum += x[idx(xx, y, z - 1)];
                    if (z + 1 < int(n)) sum += x[idx(xx, y, z + 1)];
                    out[idx(xx, y, z)] = 6.0 * self - sum;
                }
    };
    std::vector<double> b(N), x(N, 0.0), r(N), d(N), q(N);
    for (size_t i = 0; i < N; ++i) b[i] = -double(dv[i]) * h2;
    r = b;
    d = r;
    double rr = 0.0, bb = 0.0;
    for (size_t i = 0; i < N; ++i) rr += r[i] * r[i];
    bb = rr;
    int it = 0;
    for (; it < 2000 && rr > bb * 1e-14; ++it) {
        apply(d, q);
        double dq = 0.0;
        for (size_t i = 0; i < N; ++i) dq += d[i] * q[i];
        const double alpha = rr / dq;
        double rr2 = 0.0;
        for (size_t i = 0; i < N; ++i) { x[i] += alpha * d[i]; r[i] -= alpha * q[i]; rr2 += r[i] * r[i]; }
        const double beta = rr2 / rr;
        rr = rr2;
        for (size_t i = 0; i < N; ++i) d[i] = r[i] + beta * d[i];
    }
    // Gradients (cells' differences; the same boundary values as stage 5).
    auto grad = [&](auto get, int xx, int y, int z, double g[3]) {
        const double self = get(xx, y, z);
        auto at = [&](int a, int bq, int c, double outside) {
            return a < 0 || c < 0 || a >= int(n) || bq >= int(n) || c >= int(n) ? outside : bq < 0 ? self : get(a, bq, c);
        };
        g[0] = at(xx + 1, y, z, 0.0) - at(xx - 1, y, z, 0.0);
        g[1] = at(xx, y + 1, z, 0.0) - at(xx, y - 1, z, 0.0);
        g[2] = at(xx, y, z + 1, 0.0) - at(xx, y, z - 1, 0.0);
    };
    double ee = 0.0, gg = 0.0, res = 0.0;
    std::vector<double> gpu(N);
    for (size_t i = 0; i < N; ++i) gpu[i] = pr[i];
    apply(gpu, q);
    for (size_t i = 0; i < N; ++i) res += (q[i] - b[i]) * (q[i] - b[i]);
    for (int z = 0; z < int(n); ++z)
        for (int y = 0; y < int(n); ++y)
            for (int xx = 0; xx < int(n); ++xx) {
                double gr[3], gs[3];
                grad([&](int a, int bq, int c) { return x[idx(a, bq, c)]; }, xx, y, z, gr);
                grad([&](int a, int bq, int c) { return gpu[idx(a, bq, c)]; }, xx, y, z, gs);
                for (int k = 0; k < 3; ++k) { ee += (gs[k] - gr[k]) * (gs[k] - gr[k]); gg += gr[k] * gr[k]; }
            }
    Log("smoke residual: frame %llu, %u^3: |r|/|b| %.4f, gradient error %.4f of the converged one's (CG %d its)",
        (unsigned long long)g_residual.frame, n, std::sqrt(res / std::max(bb, 1e-30)), std::sqrt(ee / std::max(gg, 1e-30)), it);
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
        v->parent = 0;
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
        destroy(v.pressure);
        destroy(v.look);
        destroy(v.flow);
        v.fresh = true;
        v.age = 0.0f;
        v.n = 0;
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
    // The volumes' bounds (each made empty before its step's last stage, which widens it).
    {
        VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
        bi.size = kSmokeMax * kBoundsSlot;
        bi.usage = VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        VmaAllocationCreateInfo ac{};
        ac.usage = VMA_MEMORY_USAGE_AUTO_PREFER_DEVICE;
        if (!Check(vmaCreateBuffer(m_allocator, &bi, &ac, &m_smokeBounds, &m_smokeBoundsAllocation, nullptr), "smoke bounds",
                   error))
            return false;
    }
    // The simulation: 0, 1, 10 sampled; 2..9, 12 and 14 storage images; 11 the characters; 13 the bounds.
    {
        VkDescriptorSetLayoutBinding b[15] = {};
        for (uint32_t i = 0; i < 15; ++i)
            b[i] = {i, i < 2 || i == 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1,
                    VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
        b[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        b[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        VkDescriptorSetLayoutCreateInfo sl{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        sl.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        sl.bindingCount = 15;
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
    // The rendering: 0 the block, 1 the look (density, sunlight), 2 the flow (wisps), 3 the scene's depth, 4 the frame lights,
    // 5 the wisps' noise, 6 the bounds.
    VkDescriptorSetLayoutBinding b[7] = {};
    for (uint32_t i = 0; i < 7; ++i)
        b[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                nullptr};
    b[0].descriptorType = b[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    b[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
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
    // The half-size smoke's upsampling: 0 its colour, 1 its depths, 2 the scene's depth; push: SmokeUpPush.
    {
        VkDescriptorSetLayoutBinding u[3] = {};
        for (uint32_t i = 0; i < 3; ++i)
            u[i] = {i, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
        VkDescriptorSetLayoutCreateInfo ul{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        ul.flags = VK_DESCRIPTOR_SET_LAYOUT_CREATE_PUSH_DESCRIPTOR_BIT_KHR;
        ul.bindingCount = 3;
        ul.pBindings = u;
        if (!Check(vkCreateDescriptorSetLayout(m_device, &ul, nullptr, &m_smokeUpSetLayout), "smoke up set", error))
            return false;
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SmokeUpPush)};
        VkPipelineLayoutCreateInfo upl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        upl.setLayoutCount = 1;
        upl.pSetLayouts = &m_smokeUpSetLayout;
        upl.pushConstantRangeCount = 1;
        upl.pPushConstantRanges = &push;
        if (!Check(vkCreatePipelineLayout(m_device, &upl, nullptr, &m_smokeUpLayout), "smoke up layout", error))
            return false;
    }
    auto module = [&](const uint32_t* code, size_t size, VkShaderModule* out) {
        VkShaderModuleCreateInfo ci{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
        ci.codeSize = size;
        ci.pCode = code;
        return Check(vkCreateShaderModule(m_device, &ci, nullptr, out), "smoke module", error);
    };
    // A pipeline drawing premultiplied over its targets (the first `blended`; the others written as they are - a
    // mix needs the independent blending feature).
    auto pipeline = [&](const uint32_t* vertCode, size_t vertSize, const uint32_t* fragCode, size_t fragSize,
                        VkPipelineLayout layout, const VkFormat* formats, uint32_t count, uint32_t blended,
                        VkPipeline* out) {
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
            rs.cullMode = VK_CULL_MODE_NONE;     // (smoke.frag keeps the far faces)
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState att[5] = {};
            for (uint32_t i = 0; i < count; ++i) {
                VkPipelineColorBlendAttachmentState& a = att[i];
                a.colorWriteMask = 0xF;
                a.blendEnable = i < blended ? VK_TRUE : VK_FALSE;
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
                       "vkCreateGraphicsPipelines (smoke)", error);
        }
        if (vert) vkDestroyShaderModule(m_device, vert, nullptr);
        if (frag) vkDestroyShaderModule(m_device, frag, nullptr);
        return ok;
    };
    // The scene's targets: premultiplied over every one alike (no independent blending); the shaders give the others
    // nothing (they fade behind the smoke) - and the local fraction its cover (see smoke.frag).
    const VkFormat sceneFormats[5] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RGBA16F).vk,
                                      GetFormatInfo(Format::RGBA8).vk, GetFormatInfo(Format::RGBA16F).vk,
                                      GetFormatInfo(Format::A8R8G8B8).vk};
    // The half-size target: the smoke (premultiplied over) and the view depth each pixel marched to (blended alike -
    // no independent blending - with alpha 1: written).
    const VkFormat halfFormats[2] = {GetFormatInfo(Format::RGBA16F).vk, GetFormatInfo(Format::RG16F).vk};
    return pipeline(kSmokeVertSpirv, sizeof(kSmokeVertSpirv), kSmokeFragSpirv, sizeof(kSmokeFragSpirv), m_smokeLayout,
                    sceneFormats, 5, 5, &m_smokePipeline) &&
           pipeline(kSmokeVertSpirv, sizeof(kSmokeVertSpirv), kSmokeHalfFragSpirv, sizeof(kSmokeHalfFragSpirv),
                    m_smokeLayout, halfFormats, 2, 2, &m_smokeHalfPipeline) &&
           pipeline(kFullscreenVertSpirv, sizeof(kFullscreenVertSpirv), kSmokeUpFragSpirv, sizeof(kSmokeUpFragSpirv),
                    m_smokeUpLayout, sceneFormats, 5, 5, &m_smokeUpPipeline);
}

void Device::DestroySmokeResources()
{
    if (g_residual.buffer) { vmaDestroyBuffer(m_allocator, g_residual.buffer, g_residual.allocation); g_residual = SmokeResidual{}; }
    DestroySmokeImages();
    m_smokeVolumes.clear();
    if (m_smokeBounds) { vmaDestroyBuffer(m_allocator, m_smokeBounds, m_smokeBoundsAllocation); m_smokeBounds = VK_NULL_HANDLE; }
    for (VkPipeline* p : {&m_smokePipeline, &m_smokeHalfPipeline, &m_smokeUpPipeline})
        if (*p) { vkDestroyPipeline(m_device, *p, nullptr); *p = VK_NULL_HANDLE; }
    if (m_smokeUpLayout) { vkDestroyPipelineLayout(m_device, m_smokeUpLayout, nullptr); m_smokeUpLayout = VK_NULL_HANDLE; }
    if (m_smokeUpSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_smokeUpSetLayout, nullptr); m_smokeUpSetLayout = VK_NULL_HANDLE; }
    for (Texture** t : {&m_smokeHalf, &m_smokeHalfDepth})
        if (*t) { DestroyTexture(*t); *t = nullptr; }
    if (m_smokeSimPipeline) { vkDestroyPipeline(m_device, m_smokeSimPipeline, nullptr); m_smokeSimPipeline = VK_NULL_HANDLE; }
    if (m_smokeLayout) { vkDestroyPipelineLayout(m_device, m_smokeLayout, nullptr); m_smokeLayout = VK_NULL_HANDLE; }
    if (m_smokeSimLayout) { vkDestroyPipelineLayout(m_device, m_smokeSimLayout, nullptr); m_smokeSimLayout = VK_NULL_HANDLE; }
    if (m_smokeSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_smokeSetLayout, nullptr); m_smokeSetLayout = VK_NULL_HANDLE; }
    if (m_smokeSimSetLayout) { vkDestroyDescriptorSetLayout(m_device, m_smokeSimSetLayout, nullptr); m_smokeSimSetLayout = VK_NULL_HANDLE; }
    if (m_smokeSampler) { vkDestroySampler(m_device, m_smokeSampler, nullptr); m_smokeSampler = VK_NULL_HANDLE; }
    if (m_smokeRepeatSampler) { vkDestroySampler(m_device, m_smokeRepeatSampler, nullptr); m_smokeRepeatSampler = VK_NULL_HANDLE; }
}

// Drift clouds. A character moving through a source's box (or just past it) gets a smaller box of its own following
// them, at the source's cell size, without a source: in it the smoke they carry out through the source's walls goes on
// (smoke_sim.comp stage 9 takes over what the source's box lets go of there), stirred by them, trailing behind, fading
// with the source's life. While the character is near the source the cloud is kept; after that it follows them until
// its smoke is gone (the volume is then given back like a gone source's).
void Device::UpdateSmokeDrift(double now)
{
    if (m_smokeParams.stir <= 0.0f)
        return;
    std::vector<bool> taken(m_waterBodies.size(), false);
    // The clouds there are follow the character nearest them (each its own).
    for (SmokeVolume& d : m_smokeVolumes) {
        if (!d.key || !d.parent)
            continue;
        int best = -1;
        float bestDist = d.source.size * 0.6f;
        for (size_t i = 0; i < m_waterBodies.size(); ++i) {
            const WaterBody& b = m_waterBodies[i];
            const float dist = std::hypot(b.x - d.source.pos[0], b.z - d.source.pos[2]);
            if (!taken[i] && dist < bestDist) { best = int(i); bestDist = dist; }
        }
        if (best < 0)
            continue;                            // (left behind: stays where it is, fading)
        taken[size_t(best)] = true;
        const WaterBody& b = m_waterBodies[size_t(best)];
        d.source.pos[0] = b.x;
        d.source.pos[1] = b.minY;
        d.source.pos[2] = b.z;
        // Kept alive while its source is near (it may still hand smoke over); then its smoke's life runs out.
        for (const SmokeVolume& p : m_smokeVolumes)
            if (p.key == d.parent && std::hypot(b.x - (p.corner[0] + p.source.size * 0.5f),
                                                b.z - (p.corner[2] + p.source.size * 0.5f)) < p.source.size)
                d.lastSeen = now;
    }
    // New ones: a character moving through a source's box with none following them.
    for (size_t i = 0; i < m_waterBodies.size(); ++i) {
        const WaterBody& b = m_waterBodies[i];
        if (taken[i] || b.speed < kDriftSpeed)
            continue;
        for (size_t k = 0; k < m_smokeVolumes.size(); ++k) {
            const SmokeVolume& p = m_smokeVolumes[k];
            if (!p.key || p.parent || p.age < kWarmUp || now - p.lastSeen > 0.25)
                continue;
            const float* c = p.corner;
            const float sz = p.source.size;
            if (b.x < c[0] - 0.5f || b.x > c[0] + sz + 0.5f || b.z < c[2] - 0.5f || b.z > c[2] + sz + 0.5f ||
                b.centreY < c[1] || b.minY > c[1] + sz)
                continue;
            uint32_t count = 0;
            for (const SmokeVolume& x : m_smokeVolumes)
                if (x.key && x.parent == p.key) ++count;
            if (count >= kDriftPerSource)
                break;
            // Its volume: a free one with images of its size, else a new one, else any free one (made again - the GPU
            // waited for: a hitch).
            const float size = std::clamp(p.source.size * 0.7f, 2.5f, 5.0f);
            const uint32_t n = SmokeCells(size, m_smokeParams);
            const uint64_t parentKey = p.key;    // (p may move with the vector)
            SmokeVolume* d = nullptr;
            for (SmokeVolume& x : m_smokeVolumes)
                if (!x.key && x.n == n) { d = &x; break; }
            if (!d && m_smokeVolumes.size() < kSmokeMax)
                d = &m_smokeVolumes.emplace_back();
            for (SmokeVolume& x : m_smokeVolumes)
                if (!d && !x.key) d = &x;
            if (!d)
                break;
            d->parent = parentKey;
            const SmokeVolume& src = *std::find_if(m_smokeVolumes.begin(), m_smokeVolumes.end(),
                                                   [&](const SmokeVolume& x) { return x.key == d->parent && !x.parent; });
            static uint64_t serial = 0;
            d->key = (d->parent * 0x9E3779B97F4A7C15ull + ++serial) | (1ull << 63);   // (the game's keys: pointers)
            d->source = src.source;
            d->source.pos[0] = b.x;
            d->source.pos[1] = b.minY;
            d->source.pos[2] = b.z;
            d->source.size = size;
            d->source.rate = d->source.heat = d->source.speed = 0.0f;
            d->source.radius = 0.05f;
            d->fresh = true;
            d->age = kWarmUp;                    // (no warming up: it starts from what it is handed)
            d->simWait = 0.0f;
            d->lastStep = 0.0f;
            d->lastSeen = now;
            float wind[2];
            SmokeWind(d->source, m_rainWind, wind);
            SmokeCorner(d->source, wind, d->corner);
            taken[i] = true;
            static uint32_t logged = 0;
            if (logged < 20 && ++logged)
                Log("smoke: drift cloud %u for a character at %.1f %.1f %.1f moving %.1f/s (source %llx, %u^3 a %.1f box)",
                    logged, b.x, b.minY, b.z, b.speed, (unsigned long long)d->parent, n, size);
            break;
        }
    }
}

// The smoke: simulated, then drawn over the finished scene (EndScene: its rendering just closed).
void Device::RenderSmoke(VkCommandBuffer cmd)
{
    const SmokeParams& P = m_smokeParams;
    const double now = m_frameClock;
    const float dt = m_smokeLastClock < 0.0 ? 0.0f : float(std::clamp(now - m_smokeLastClock, 0.0, 1.0 / 30.0));
    m_smokeLastClock = now;
    if (g_residual.readAt && m_frameNumber >= g_residual.readAt)
        LogSmokeResidual();
    if (!P.enable || !m_smokeSimPipeline || !m_smokePipeline || !m_scene || !m_hdr || m_smokeVolumes.empty())
        return;
    UpdateSmokeDrift(now);
    // Gone sources whose smoke has faded give their volumes back.
    for (SmokeVolume& v : m_smokeVolumes)
        if (v.key && now - v.lastSeen > double(v.source.life) * 1.5 + 1.0)
            v.key = 0;
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
    // Each volume's cells a side: RVK_SmokeRes per world unit of its box (a long-lived source's tall box keeps its
    // detail), a multiple of 4, at most RVK_SmokeQual's. A volume whose count changed, or a scratch too small for
    // one, is made again - after the GPU is done with the old (rare: a source's box or the settings changed).
    auto cells = [&](const SmokeVolume& v) { return SmokeCells(v.source.size, P); };
    uint32_t scratch = 0;
    bool remake = false;
    for (auto& [d, v] : live) {
        if (v->n && v->n != cells(*v)) remake = true;
        scratch = std::max(scratch, cells(*v));
    }
    if (scratch > m_smokeN && m_smokeN) remake = true;
    if (remake) {
        vkDeviceWaitIdle(m_device);
        auto destroy = [&](SmokeImage& i) {
            if (i.view) vkDestroyImageView(m_device, i.view, nullptr);
            if (i.image) vmaDestroyImage(m_allocator, i.image, i.allocation);
            i = SmokeImage{};
        };
        for (auto& [d, v] : live) {
            if (v->n && v->n != cells(*v)) {
                for (int k = 0; k < 2; ++k) { destroy(v->vel[k]); destroy(v->dens[k]); }
                destroy(v->pressure);
                destroy(v->flow);
                destroy(v->look);
                v->n = 0;
                v->fresh = true;
            }
        }
        if (scratch > m_smokeN) {
            destroy(m_smokeCurl);
            destroy(m_smokeDiv);
            destroy(m_smokePressure[0]);
            destroy(m_smokePressure[1]);
        }
    }
    m_smokeN = std::max(m_smokeN, scratch);
    for (auto& [d, v] : live)
        if (!v->n)
            v->n = cells(*v);

    // Images: the shared scratch, and each volume's on first use.
    const VkFormat rgba = VK_FORMAT_R16G16B16A16_SFLOAT, r32 = VK_FORMAT_R32_SFLOAT, r16 = VK_FORMAT_R16_SFLOAT;
    std::vector<VkImageMemoryBarrier2> toGeneral;
    std::vector<VkImage> toClear;
    auto ensure = [&](SmokeImage& img, VkFormat f, bool clear, uint32_t n) {
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
    const uint32_t ns = m_smokeN;
    bool ok = ensure(m_smokeCurl, rgba, false, ns) && ensure(m_smokeDiv, r32, false, ns) &&
              ensure(m_smokePressure[0], r32, false, ns) && ensure(m_smokePressure[1], r32, false, ns);
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
        ok = ok && ensure(v->vel[0], rgba, true, v->n) && ensure(v->vel[1], rgba, true, v->n) &&
             ensure(v->dens[0], rgba, true, v->n) && ensure(v->dens[1], rgba, true, v->n) && ensure(v->look, rgba, true, v->n) &&
             ensure(v->flow, VK_FORMAT_R8G8B8A8_SNORM, true, v->n / 2) &&
             (!P.exact || ensure(v->pressure, r32, true, v->n));
        if (v->fresh) {
            for (int k = 0; k < 2; ++k) { toClear.push_back(v->vel[k].image); toClear.push_back(v->dens[k].image); }
            toClear.push_back(v->look.image);
            toClear.push_back(v->flow.image);
            if (v->pressure.image) toClear.push_back(v->pressure.image);
        }
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
    memory(VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
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
    uint32_t groups = 0;
    EnsureRingSpace(live.size() * (sizeof(SmokeBodies) + 256) + 1024);
    Frame& f = m_frames[m_frameIndex];
    // Which volumes step this frame: warming up ones every frame; of the others those due, the most overdue first, as
    // many as keep them all stepping every kSimStep or so - each frame about the same work, not two volumes' steps in
    // one frame and none in the next (a stutter) - and any a period late.
    std::vector<uint8_t> stepNow(live.size()), warmingNow(live.size());
    {
        std::vector<std::pair<float, size_t>> due;
        size_t stepping = 0;
        for (size_t i = 0; i < live.size(); ++i) {
            SmokeVolume& v = *live[i].second;
            warmingNow[i] = v.age < kWarmUp;
            v.age += dt;
            v.simWait += dt;
            const float period = warmingNow[i] ? 0.0f : live[i].first > kSimFar ? 2.0f * kSimStep : kSimStep;
            if (warmingNow[i] || !m_smokeNoiseMade || v.simWait >= 2.0f * period) {
                stepNow[i] = 1;
                ++stepping;
            } else if (v.simWait + 0.5f * dt >= period) {
                due.emplace_back(v.simWait / period, i);
            }
        }
        std::sort(due.begin(), due.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
        const size_t budget = std::max<size_t>(1, size_t(std::ceil(float(live.size()) * dt / kSimStep)));
        for (const auto& [late, i] : due)
            if (stepping < budget) { stepNow[i] = 1; ++stepping; }
    }
    for (size_t li = 0; li < live.size(); ++li) {
        auto& [dist, vp] = live[li];
        SmokeVolume& v = *vp;
        const SmokeSource& s = v.source;
        const uint32_t n = v.n;
        groups = n / 4;
        if (!stepNow[li])
            continue;
        const bool warming = warmingNow[li];
        const float simDt = std::min(v.simWait, 1.0f / 30.0f);   // (the step's dt: what has passed since the last)
        v.lastStep = simDt;
        v.simWait = 0.0f;
        const uint32_t slot = uint32_t(&v - m_smokeVolumes.data());
        vkCmdFillBuffer(cmd, m_smokeBounds, slot * kBoundsSlot, 16, 0xFFFFFFFFu);   // (empty: lowest > highest)
        vkCmdFillBuffer(cmd, m_smokeBounds, slot * kBoundsSlot + 16, 16, 0u);
        memory(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_WRITE_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
               VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
        const bool fed = now - v.lastSeen < 0.25;           // a source gone: its smoke goes on, nothing added
        float corner[3], shift[3], wind[2];
        SmokeWind(s, m_rainWind, wind);
        SmokeCorner(s, wind, corner);
        for (int i = 0; i < 3; ++i) {
            shift[i] = (corner[i] - v.corner[i]) / s.size;
            v.corner[i] = corner[i];
        }
        const float step = warming ? simDt * kWarmSpeed : simDt;
        SimPush p{};
        p.a[1] = float(n);
        p.a[2] = step;
        p.a[3] = s.size;
        for (int i = 0; i < 3; ++i)
            p.b[i] = (s.pos[i] - corner[i]) / s.size;
        p.b[3] = s.radius / s.size;
        const float puff = Puff(now, v.key);
        // (A small source denser, so it gives about as much smoke; RVK_SmokeDensity is the extinction's, not this.)
        const float small = std::clamp(0.6f / std::max(s.radius, 0.05f), 1.0f, 4.0f);
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
        p.f[3] = kExtinction * P.density;                     // (as thick to the sun as it is to the eye)
        p.g[0] = float(slot);
        p.g[1] = 1.0f / kFlowMax;
        p.g[2] = kEdgeFade;
        // A drift cloud: its source's box (stage 9 takes the smoke that box lets go of): this box's coordinates ->
        // that one's, uvw * h.w + h.xyz; its density read through binding 10.
        const SmokeVolume* parent = nullptr;
        if (v.parent)
            for (const auto& [pd, pv] : live)
                if (pv->key == v.parent && !pv->parent && pv->n) parent = pv;
        if (parent) {
            for (int i = 0; i < 3; ++i)
                p.h[i] = (corner[i] - parent->corner[i]) / parent->source.size;
            p.h[3] = s.size / parent->source.size;
        }
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
        // Bindings: 0 velIn, 1 densIn (sampled); 2 velOut, 3 densOut, 4 curl, 5 pIn, 6 pOut, 7 div, 8 velRead, 9 look;
        // 10 the curl image sampled (MacCormack's forward step); 11 the characters; 12 the wisps' noise.
        VkImageView forwardView = m_smokeCurl.view;   // binding 10: the curl image (stage 8); stage 7 the last state
        auto dispatch = [&](int stage, const SmokeImage& velIn, const SmokeImage& densIn, const SmokeImage& velOut,
                            const SmokeImage& densOut, const SmokeImage& pIn, const SmokeImage& pOut,
                            const SmokeImage& velRead, float e3) {
            VkDescriptorImageInfo info[15] = {
                {m_smokeSampler, velIn.view, VK_IMAGE_LAYOUT_GENERAL},  {m_smokeSampler, densIn.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, velOut.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, densOut.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, m_smokeCurl.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, pIn.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, pOut.view, VK_IMAGE_LAYOUT_GENERAL},   {VK_NULL_HANDLE, m_smokeDiv.view, VK_IMAGE_LAYOUT_GENERAL},
                {VK_NULL_HANDLE, velRead.view, VK_IMAGE_LAYOUT_GENERAL}, {VK_NULL_HANDLE, v.look.view, VK_IMAGE_LAYOUT_GENERAL},
                {m_smokeSampler, forwardView, VK_IMAGE_LAYOUT_GENERAL}, {}, {VK_NULL_HANDLE, m_smokeNoise.view, VK_IMAGE_LAYOUT_GENERAL},
                {}, {VK_NULL_HANDLE, v.flow.view, VK_IMAGE_LAYOUT_GENERAL},
            };
            VkDescriptorBufferInfo boundsInfo{m_smokeBounds, 0, VK_WHOLE_SIZE};
            VkWriteDescriptorSet w[15] = {};
            for (uint32_t i = 0; i < 15; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = i < 2 || i == 10 ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                w[i].pImageInfo = &info[i];
            }
            w[11].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
            w[11].pImageInfo = nullptr;
            w[11].pBufferInfo = &bodiesInfo;
            w[13].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
            w[13].pImageInfo = nullptr;
            w[13].pBufferInfo = &boundsInfo;
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_smokeSimLayout, 0, 15, w);
            SimPush q = p;
            q.a[0] = float(stage);
            q.e[3] = e3;
            vkCmdPushConstants(cmd, m_smokeSimLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(q), &q);
            vkCmdDispatch(cmd, groups, groups, groups);
            memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
                   VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_READ_BIT | VK_ACCESS_2_SHADER_SAMPLED_READ_BIT);
            // (RANDYVK_SMOKE_STAGES: the profile times each stage, all volumes' summed.)
            static const char* const kStageNames[] = {"smoke 0 advect", "smoke 1 curl", "smoke 2 vorticity",
                                                      "smoke 3 divergence", "smoke 4 pressure", "smoke 5 project",
                                                      "smoke 6 density on", "smoke 7 light", "smoke 8 density back",
                                                      "smoke 9 density", "smoke 10 noise", "smoke 11 flow"};
            static const bool stageProfile = getenv("RANDYVK_SMOKE_STAGES") != nullptr;
            if (stageProfile)
                ProfileMark(kStageNames[stage]);
        };
        // The pressure: Jacobi steps between the first image and the second; the first the volume's own (RVK_SmokeExact:
        // kept, the next solve starts from it) or shared (the solve starts from 0).
        SmokeImage pr[2] = {P.exact ? v.pressure : m_smokePressure[0], m_smokePressure[1]};
        const uint32_t jacobi = P.exact ? kJacobiExact : kJacobi;
        if (!m_smokeNoiseMade) {                 // (once: the wisps' noise)
            p.a[1] = float(kNoiseSize);
            groups = kNoiseSize / 4;
            dispatch(10, v.vel[c], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], 0.0f);
            p.a[1] = float(n);
            groups = n / 4;
            m_smokeNoiseMade = true;
        }
        dispatch(0, v.vel[c], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], float(m_frameNumber % 65521u));
        dispatch(1, v.vel[c], v.dens[c], v.vel[c], v.dens[o], pr[0], pr[1], v.vel[o], 0.0f);   // curl of vel[o]
        dispatch(2, v.vel[o], v.dens[c], v.vel[c], v.dens[o], pr[0], pr[1], v.vel[o], 0.0f);   // vorticity -> vel[c]
        dispatch(3, v.vel[o], v.dens[c], v.vel[o], v.dens[o], pr[0], pr[1], v.vel[c], 0.0f);   // divergence of vel[c]
        for (uint32_t k = 0; k < jacobi; ++k)                                                   // pressure: pr[0] <-> pr[1]
            dispatch(4, v.vel[o], v.dens[c], v.vel[o], v.dens[o], pr[k & 1], pr[(k & 1) ^ 1], v.vel[c],
                     k == 0 && !P.exact ? 1.0f : 0.0f);
        const SmokeImage& p0 = pr[jacobi & 1];                                                 // (the last written)
        static const uint64_t residualFrame = getenv("RANDYVK_SMOKE_RESIDUAL") ? std::strtoull(getenv("RANDYVK_SMOKE_RESIDUAL"), nullptr, 10) : 0;
        if (residualFrame && m_frameNumber == residualFrame && &v == live.front().second) {
            const VkDeviceSize bytes = VkDeviceSize(n) * n * n * 4;
            if (!g_residual.buffer) {
                VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
                bi.size = bytes * 2;
                bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
                VmaAllocationCreateInfo rc{};
                rc.usage = VMA_MEMORY_USAGE_AUTO;
                rc.flags = VMA_ALLOCATION_CREATE_HOST_ACCESS_RANDOM_BIT | VMA_ALLOCATION_CREATE_MAPPED_BIT;
                VmaAllocationInfo info{};
                if (vmaCreateBuffer(m_allocator, &bi, &rc, &g_residual.buffer, &g_residual.allocation, &info) == VK_SUCCESS)
                    g_residual.cpu = info.pMappedData;
            }
            if (g_residual.cpu) {
                memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT, VK_PIPELINE_STAGE_2_TRANSFER_BIT,
                       VK_ACCESS_2_TRANSFER_READ_BIT);
                VkBufferImageCopy r{};
                r.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                r.imageExtent = {n, n, n};
                vkCmdCopyImageToBuffer(cmd, p0.image, VK_IMAGE_LAYOUT_GENERAL, g_residual.buffer, 1, &r);
                r.bufferOffset = bytes;
                vkCmdCopyImageToBuffer(cmd, m_smokeDiv.image, VK_IMAGE_LAYOUT_GENERAL, g_residual.buffer, 1, &r);
                memory(VK_PIPELINE_STAGE_2_TRANSFER_BIT, VK_ACCESS_2_TRANSFER_READ_BIT, VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT,
                       VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT);
                g_residual.frame = m_frameNumber;
                g_residual.readAt = m_frameNumber + 6;
                g_residual.n = n;
                g_residual.h = s.size / float(n);
            }
        }
        dispatch(5, v.vel[c], v.dens[c], v.vel[o], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // projected -> vel[o]
        dispatch(6, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // density forward -> curl
        dispatch(8, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // ... and back -> div
        if (parent)
            forwardView = parent->dens[parent->cur].view;   // (stage 9: the source's smoke, for a drift cloud)
        dispatch(9, v.vel[o], v.dens[c], v.vel[c], v.dens[o], p0, pr[0], v.vel[c], 0.0f);       // corrected -> dens[o]
        forwardView = m_smokeCurl.view;
        forwardView = v.dens[c].view;            // (the state before this step, for the look's blend from it)
        dispatch(7, v.vel[o], v.dens[o], v.vel[c], v.dens[c], p0, pr[0], v.vel[c], 0.0f);       // sunlight -> look
        forwardView = m_smokeCurl.view;
        groups = (n / 2 + 3) / 4;
        dispatch(11, v.vel[o], v.dens[o], v.vel[c], v.dens[c], p0, pr[0], v.vel[c], 0.0f);      // vel[o] -> flow
        v.cur = o;
    }
    memory(VK_PIPELINE_STAGE_2_COMPUTE_SHADER_BIT, VK_ACCESS_2_SHADER_STORAGE_WRITE_BIT,
           VK_PIPELINE_STAGE_2_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_2_FRAGMENT_SHADER_BIT,
           VK_ACCESS_2_SHADER_SAMPLED_READ_BIT | VK_ACCESS_2_SHADER_STORAGE_READ_BIT);
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
    // RVK_SmokeHalf: marched into a half-size target (cleared), then upsampled over the scene's (smoke_up.frag).
    const uint32_t fw = m_scene->m_width, fh = m_scene->m_height, hw = (fw + 1) / 2, hh = (fh + 1) / 2;
    if (P.half && (!m_smokeHalf || m_smokeHalf->m_width != hw || m_smokeHalf->m_height != hh)) {
        for (Texture** t : {&m_smokeHalf, &m_smokeHalfDepth})
            if (*t) { DestroyTexture(*t); *t = nullptr; }
        m_smokeHalf = CreateImage(hw, hh, Format::RGBA16F, 1, true);
        m_smokeHalfDepth = CreateImage(hw, hh, Format::RG16F, 1, true);
    }
    const bool half = P.half && m_smokeHalf && m_smokeHalfDepth;
    Texture* targets[5] = {m_scene, m_glow, m_localFraction, m_motionVectors, m_albedo};
    Texture* halfTargets[2] = {m_smokeHalf, m_smokeHalfDepth};
    auto beginRendering = [&](Texture* const* t, uint32_t count, bool clear) {
        VkRenderingAttachmentInfo colors[5] = {};
        for (uint32_t i = 0; i < count; ++i) {
            Transition(cmd, t[i], VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL);
            colors[i].sType = VK_STRUCTURE_TYPE_RENDERING_ATTACHMENT_INFO;
            colors[i].imageView = t[i]->m_view;
            colors[i].imageLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
            colors[i].loadOp = clear ? VK_ATTACHMENT_LOAD_OP_CLEAR : VK_ATTACHMENT_LOAD_OP_LOAD;
            colors[i].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        }
        VkRenderingInfo ri{VK_STRUCTURE_TYPE_RENDERING_INFO};
        ri.renderArea = {{0, 0}, {t[0]->m_width, t[0]->m_height}};
        ri.layerCount = 1;
        ri.colorAttachmentCount = count;
        ri.pColorAttachments = colors;
        vkCmdBeginRendering(cmd, &ri);
    };
    beginRendering(half ? halfTargets : targets, half ? 2 : 5, half);
    // The scene draws into a flipped viewport (half size: the same, halved).
    const float sw = float(fw), sh = float(fh), k = half ? 0.5f : 1.0f;
    VkViewport viewport{0.5f * k, (sh + 0.5f) * k, sw * k, -sh * k, 0.0f, 1.0f};
    VkRect2D scissor{{0, 0}, {half ? hw : fw, half ? hh : fh}};
    vkCmdSetViewport(cmd, 0, 1, &viewport);
    vkCmdSetScissor(cmd, 0, 1, &scissor);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, half ? m_smokeHalfPipeline : m_smokePipeline);
    const d3d::Matrix viewProj = MulMatrix(m_view, m_proj);
    float screenLo[2] = {sw, sh}, screenHi[2] = {0.0f, 0.0f};   // where the boxes are on screen (the upsampling's)
    // Far ones first (each blends over what is behind it).
    std::sort(live.begin(), live.end(), [](const auto& a, const auto& b) { return a.first > b.first; });
    for (auto& [dist, vp] : live) {
        SmokeVolume& v = *vp;
        SmokeFrame sf{};
        sf.viewProj = viewProj;
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
        sf.look[0] = std::clamp(P.opacity, 0.0f, 8.0f);
        sf.look[1] = v.lastStep > 0.0f ? std::clamp(v.simWait / v.lastStep, 0.0f, 1.0f) : 1.0f;
        sf.info[0] = uint32_t(&v - m_smokeVolumes.data());
        sf.info[2] = v.n;
        for (int c = 0; c < 8; ++c) {
            const float p[3] = {v.corner[0] + (c & 1 ? v.source.size : 0.0f), v.corner[1] + (c & 2 ? v.source.size : 0.0f),
                                v.corner[2] + (c & 4 ? v.source.size : 0.0f)};
            float clip[4];
            for (int j = 0; j < 4; ++j)
                clip[j] = p[0] * viewProj.m[0][j] + p[1] * viewProj.m[1][j] + p[2] * viewProj.m[2][j] + viewProj.m[3][j];
            if (clip[3] < 0.05f) {               // (round the camera: anywhere)
                screenLo[0] = screenLo[1] = 0.0f;
                screenHi[0] = sw;
                screenHi[1] = sh;
                continue;
            }
            const float x = 0.5f + (clip[0] / clip[3] + 1.0f) * 0.5f * sw, y = sh + 0.5f - (clip[1] / clip[3] + 1.0f) * 0.5f * sh;
            screenLo[0] = std::min(screenLo[0], x);
            screenLo[1] = std::min(screenLo[1], y);
            screenHi[0] = std::max(screenHi[0], x);
            screenHi[1] = std::max(screenHi[1], y);
        }
        // The lamps whose light reaches the box (point and spot; the nearest to its middle if more).
        {
            std::pair<float, uint32_t> reach[kFrameLights];
            uint32_t count = 0;
            const float half = v.source.size * 0.5f;
            for (uint32_t k = 0; k < m_frameLightSpheres.size() && k < m_frameLightIndices.size(); ++k) {
                const d3d::Light& l = m_lightsPrev[m_frameLightIndices[k]].light;
                if (l.type != d3d::LIGHT_POINT && l.type != d3d::LIGHT_SPOT)
                    continue;
                const LightSphere& sp = m_frameLightSpheres[k];
                const float at[3] = {sp.x, sp.y, sp.z};
                float outside = 0.0f, centre = 0.0f;
                for (int i = 0; i < 3; ++i) {
                    const float c = v.corner[i] + half, d = std::max(std::fabs(at[i] - c) - half, 0.0f);
                    outside += d * d;
                    centre += (at[i] - c) * (at[i] - c);
                }
                if (outside < sp.r2)
                    reach[count++] = {centre, k};
            }
            const uint32_t used = std::min(count, kLamps);
            std::partial_sort(reach, reach + used, reach + count, [](const auto& a, const auto& b) { return a.first < b.first; });
            for (uint32_t i = 0; i < used; ++i) sf.lamps[i] = reach[i].second;
            sf.info[1] = used;
        }
        sf.detail[0] = P.detail;
        sf.detail[1] = 1.2f;
        sf.detail[2] = 2.5f;
        sf.detail[3] = float(v.key % 101u) * 0.137f;
        // (Fading in while it warms up; out over the last second before its volume is given back.)
        const float ending = float(double(v.source.life) * 1.5 + 1.0 - (now - v.lastSeen));
        sf.colour[3] = kExtinction * P.density * std::clamp(v.age / kWarmUp, 0.0f, 1.0f) * std::clamp(ending, 0.0f, 1.0f);
        if (m_terrainAmbient[3] > 0.5f)
            std::memcpy(sf.ambient, m_terrainAmbient, 12);
        else
            ArgbTo4(m_rs[d3d::RS_AMBIENT], sf.ambient);
        sf.ambient[3] = std::clamp(float(v.n) * 0.75f, 32.0f, 96.0f);   // (the march's steps: about a cell each)
        sf.proj[0] = m_proj.m[2][2];
        sf.proj[1] = m_proj.m[3][2];
        sf.proj[2] = m_proj.m[0][0];
        sf.proj[3] = m_proj.m[1][1];
        sf.viewport[0] = sw;
        sf.viewport[1] = sh;
        sf.viewport[2] = 1.0f / sw;
        sf.viewport[3] = 1.0f / sh;
        SceneFog(sf.fogColour, sf.fogParams);   // (the scene's: the render states hold the interface's by now)
        void* cpu = nullptr;
        const VkDeviceSize offset = Allocate(sizeof(SmokeFrame), 256, &cpu);
        std::memcpy(cpu, &sf, sizeof(sf));
        VkDescriptorBufferInfo blockInfo{f.ring, offset, sizeof(SmokeFrame)};
        VkDescriptorBufferInfo lightsInfo{f.ring, m_frameLightsOffset, sizeof(FrameLights)};
        VkDescriptorImageInfo lookInfo{m_smokeSampler, v.look.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo flowInfo{m_smokeSampler, v.flow.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorImageInfo depthInfo{m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo noiseInfo{m_smokeRepeatSampler, m_smokeNoise.view, VK_IMAGE_LAYOUT_GENERAL};
        VkDescriptorBufferInfo boundsInfo{m_smokeBounds, 0, VK_WHOLE_SIZE};
        VkWriteDescriptorSet w[7] = {};
        for (uint32_t i = 0; i < 7; ++i) {
            w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
            w[i].dstBinding = i;
            w[i].descriptorCount = 1;
            w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        }
        w[0].descriptorType = w[4].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        w[0].pBufferInfo = &blockInfo;
        w[1].pImageInfo = &lookInfo;
        w[2].pImageInfo = &flowInfo;
        w[3].pImageInfo = &depthInfo;
        w[4].pBufferInfo = &lightsInfo;
        w[5].pImageInfo = &noiseInfo;
        w[6].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
        w[6].pBufferInfo = &boundsInfo;
        vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokeLayout, 0, 7, w);
        vkCmdDraw(cmd, 36, 1, 0, 0);
    }
    vkCmdEndRendering(cmd);
    if (half) {
        // Upsampled over the scene's targets where the boxes are: each pixel from the four half-size ones round it,
        // weighed by how near their depths are to its own (an edge keeps the smoke on its side of it).
        for (Texture* t : halfTargets)
            Transition(cmd, t, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL);
        const int x0 = std::max(int(std::floor(screenLo[0])) - 3, 0), y0 = std::max(int(std::floor(screenLo[1])) - 3, 0);
        const int x1 = std::min(int(std::ceil(screenHi[0])) + 3, int(fw)), y1 = std::min(int(std::ceil(screenHi[1])) + 3, int(fh));
        if (x1 > x0 && y1 > y0) {
            beginRendering(targets, 5, false);
            VkViewport full{0.0f, 0.0f, sw, sh, 0.0f, 1.0f};
            VkRect2D area{{x0, y0}, {uint32_t(x1 - x0), uint32_t(y1 - y0)}};
            vkCmdSetViewport(cmd, 0, 1, &full);
            vkCmdSetScissor(cmd, 0, 1, &area);
            vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokeUpPipeline);
            VkDescriptorImageInfo images[3] = {
                {m_pointSampler, m_smokeHalf->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {m_pointSampler, m_smokeHalfDepth->m_view, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL},
                {m_pointSampler, m_depthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL}};
            VkWriteDescriptorSet w[3] = {};
            for (uint32_t i = 0; i < 3; ++i) {
                w[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                w[i].dstBinding = i;
                w[i].descriptorCount = 1;
                w[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
                w[i].pImageInfo = &images[i];
            }
            vkCmdPushDescriptorSetKHR(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, m_smokeUpLayout, 0, 3, w);
            SmokeUpPush push{{m_proj.m[2][2], m_proj.m[3][2], float(hw), float(hh)}};
            vkCmdPushConstants(cmd, m_smokeUpLayout, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), &push);
            vkCmdDraw(cmd, 3, 1, 0, 0);
            vkCmdEndRendering(cmd);
        }
    }
    ProfileMark("smoke");
}

}  // namespace rvk
