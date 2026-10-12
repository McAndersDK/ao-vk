// The game's smoke effects, simulated by rvk instead (RVK_SmokeOn; rvk/smoke.cpp).
//
// A smoke effect is a _GfxControlSmoke_t (Gamecode): each frame its update (vtable slot 1) moves what it is attached
// to, emits sprites into its GfxVisualSprite2Type0 and moves them. That slot is pointed here: after the game's update,
// the effect becomes a smoke source for rvk (where it is, its colours, its sprites' sizes, speed and life - keyed by
// the effect, handed over each frame it runs) and its sprites are not drawn (GfxVisual::DisableRendering; the game sets
// it again each update, so it is set after).
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <unordered_set>

namespace rvkproxy {

namespace {

// _GfxControlSmoke_t (Gamecode: its vtable, the update in slot 1 - FUN_100eff68).
constexpr uint32_t kSmokeVtable = 0x1016CAF4 - 0x10000000, kSmokeUpdateSlot = 1;
constexpr uint32_t kSmokeUpdate = 0x100EFF68 - 0x10000000;
// Its fields (the update; _GfxControl_t::Process).
constexpr size_t kEnded = 0x14;                  // byte: the effect is over
constexpr size_t kAttach = 0x2c;                 // what it is attached to: its world position at +0xc
constexpr size_t kAttachPos = 0xc;
constexpr size_t kVisual = 0x30;                 // its GfxVisualSprite2Type0
constexpr size_t kRadius = 0x40;                 // two floats: where sprites start, min / max (from its centre)
constexpr size_t kSizes = 0x4c;                  // four floats: sprite size at birth min / max, at death min / max
constexpr size_t kColourStart = 0x5c;            // four floats: RGBA at birth
constexpr size_t kColourEnd = 0x6c;              // ... at death
constexpr size_t kLife = 0x7c;                   // float: a sprite's life (s)
constexpr size_t kSpeed = 0x90;                  // two floats: a sprite's speed, min / max

using UpdateFn = void(__thiscall*)(void* self);
using DisableFn = void(__thiscall*)(void* visual, int disable);
UpdateFn g_update;
DisableFn g_disable;
bool g_installed, g_gaveUp;
std::unordered_set<uintptr_t> g_logged;

template <typename T>
T At(const void* p, size_t offset)
{
    T v;
    std::memcpy(&v, static_cast<const uint8_t*>(p) + offset, sizeof(T));
    return v;
}

uint32_t Channel(float c)                         // a colour channel (0..1, or 0..255 as some effects give it)
{
    if (c > 1.5f) c /= 255.0f;
    return uint32_t(std::clamp(c, 0.0f, 1.0f) * 255.0f + 0.5f);
}

void __fastcall SmokeUpdate(void* self, void*)
{
    g_update(self);
    rvk::ThreadedDevice* device = g_rvk.device;
    if (!device || rvk_settings::GetEffective("RVK_SmokeOn") == 0.0f || At<uint8_t>(self, kEnded))
        return;
    const void* attach = At<const void*>(self, kAttach);
    void* visual = At<void*>(self, kVisual);
    if (!attach || !visual)
        return;
    float pos[3], radius[2], sizes[4], c0[4], c1[4], speed[2];
    std::memcpy(pos, static_cast<const uint8_t*>(attach) + kAttachPos, sizeof(pos));
    std::memcpy(radius, static_cast<const uint8_t*>(self) + kRadius, sizeof(radius));
    std::memcpy(sizes, static_cast<const uint8_t*>(self) + kSizes, sizeof(sizes));
    std::memcpy(c0, static_cast<const uint8_t*>(self) + kColourStart, sizeof(c0));
    std::memcpy(c1, static_cast<const uint8_t*>(self) + kColourEnd, sizeof(c1));
    std::memcpy(speed, static_cast<const uint8_t*>(self) + kSpeed, sizeof(speed));
    const float life = At<float>(self, kLife);
    if (!std::isfinite(pos[0]) || !std::isfinite(pos[1]) || !std::isfinite(pos[2]))
        return;
    if (g_logged.size() < 24 && g_logged.insert(uintptr_t(self)).second)
        RvkLog("smoke: effect %p at %.1f %.1f %.1f: radius %.2f..%.2f, sizes %.2f..%.2f -> %.2f..%.2f, colour %.2f %.2f "
               "%.2f %.2f -> %.2f %.2f %.2f %.2f, life %.2f, speed %.2f..%.2f", self, pos[0], pos[1], pos[2], radius[0],
               radius[1], sizes[0], sizes[1], sizes[2], sizes[3], c0[0], c0[1], c0[2], c0[3], c1[0], c1[1], c1[2], c1[3],
               life, speed[0], speed[1]);
    // The source: as wide as its sprites start; rising as fast as they go; lasting as they do; as dense as they are
    // opaque; its box tall enough for the smoke to rise through it in that time.
    rvk::Device::SmokeSource s;
    std::memcpy(s.pos, pos, sizeof(pos));
    const float startSize = std::isfinite(sizes[0] + sizes[1]) ? 0.5f * (sizes[0] + sizes[1]) : 1.0f;
    const float rise = std::isfinite(speed[0] + speed[1]) ? 0.5f * (speed[0] + speed[1]) : 1.0f;
    s.radius = std::clamp(0.4f * startSize + 0.5f * std::max(radius[1], 0.0f), 0.25f, 2.5f);
    s.speed = std::clamp(rise, 0.3f, 4.0f);
    s.life = std::isfinite(life) ? std::clamp(life, 1.0f, 12.0f) : 4.0f;
    s.size = std::clamp(s.speed * s.life * 0.8f + 4.0f * s.radius, 4.0f, 20.0f);
    s.rate = 1.5f * std::clamp(std::isfinite(c0[3]) ? (c0[3] > 1.5f ? c0[3] / 255.0f : c0[3]) : 0.6f, 0.15f, 1.0f);
    s.colour = Channel(c0[0]) << 16 | Channel(c0[1]) << 8 | Channel(c0[2]);
    device->SmokeSourceFrame(uint64_t(uintptr_t(self)), s);
    g_disable(visual, 1);                         // (the game's sprites: ours instead)
}

}  // namespace

// Each presented frame: points _GfxControlSmoke_t's update here once Gamecode and DisplaySystem are loaded.
void SmokeFrame()
{
    if (g_installed || g_gaveUp)
        return;
    HMODULE gc = GetModuleHandleA("Gamecode.dll"), ds = GetModuleHandleA("DisplaySystem.dll");
    if (!gc || !ds)
        return;
    g_disable = reinterpret_cast<DisableFn>(GetProcAddress(ds, "?DisableRendering@GfxVisual@@QAEXH@Z"));
    void* update = nullptr;
    if (!g_disable || !PatchSlot(gc, kSmokeVtable, kSmokeUpdateSlot, kSmokeUpdate, reinterpret_cast<void*>(&SmokeUpdate),
                                 &update, "smoke: _GfxControlSmoke_t update")) {
        g_gaveUp = true;
        return;
    }
    g_update = reinterpret_cast<UpdateFn>(update);
    g_installed = true;
    RvkLog("smoke: _GfxControlSmoke_t update hooked (Gamecode %p)", (void*)gc);
}

}  // namespace rvkproxy
