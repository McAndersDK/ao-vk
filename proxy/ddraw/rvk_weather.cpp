// The game's weather visuals, drawn by rvk instead (RVK_WeatherOn; rvk/rain.cpp).
//
// Gamecode's weather objects keep a VisualRain_t (DisplaySystem) - a few hundred lines in a box round the camera -
// and each frame they are on call VisualRain_t::process(dt, camera position, orientation, wind, intensity) through
// their import of it. That import is pointed here: what the game asks for (the intensity and wind, and from the
// object its drops' colour, fall speed, wind factor and opacity) goes to rvk, and the game's own call is made with
// intensity 0, which hides its lines (process: "intensity <= 0: invisible").
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <cstdint>
#include <cstring>

namespace rvkproxy {

namespace {

// VisualRain_t::process(float, Vector3_t, Quaternion_t, Vector3_t, float) - __thiscall; the vectors by value.
using ProcessFn = void(__thiscall*)(void* self, float dt, float px, float py, float pz, float qx, float qy, float qz,
                                    float qw, float wx, float wy, float wz, float intensity);
ProcessFn g_process;
bool g_installed, g_gaveUp;

// VisualRain_t's fields (DisplaySystem: VisualRain_t::SetColor, its constructor, FUN_10052889).
constexpr size_t kColourR = 0x34, kColourG = 0x35, kColourB = 0x36;   // bytes
constexpr size_t kFallSpeed = 0x24, kWindScale = 0x38, kOpacity = 0x5bc;   // floats

float FieldF(const void* self, size_t offset)
{
    float f;
    std::memcpy(&f, static_cast<const uint8_t*>(self) + offset, 4);
    return f;
}

// (__fastcall with an unused second register: the __thiscall layout - self in ECX, the rest on the stack, popped
// here.)
void __fastcall RainProcess(void* self, void*, float dt, float px, float py, float pz, float qx, float qy, float qz,
                            float qw, float wx, float wy, float wz, float intensity)
{
    rvk::ThreadedDevice* device = g_rvk.device;
    if (!device || !self || rvk_settings::GetEffective("RVK_WeatherOn") == 0.0f) {
        g_process(self, dt, px, py, pz, qx, qy, qz, qw, wx, wy, wz, intensity);
        return;
    }
    if (intensity > 0.0f) {
        const uint8_t* b = static_cast<const uint8_t*>(self);
        rvk::Device::WeatherState s;
        s.precipitation = intensity;
        s.wind[0] = wx;
        s.wind[1] = wy;
        s.wind[2] = wz;
        s.colour = uint32_t(b[kColourR]) << 16 | uint32_t(b[kColourG]) << 8 | b[kColourB];
        s.fallSpeed = FieldF(self, kFallSpeed);
        s.windScale = FieldF(self, kWindScale);
        s.opacity = FieldF(self, kOpacity);
        device->SetWeather(s);
    }
    g_process(self, dt, px, py, pz, qx, qy, qz, qw, wx, wy, wz, 0.0f);   // (hides the game's lines)
}

}  // namespace

// Each presented frame: points Gamecode's import of VisualRain_t::process here once Gamecode is loaded.
void WeatherFrame()
{
    if (g_installed || g_gaveUp)
        return;
    HMODULE gc = GetModuleHandleA("Gamecode.dll");
    if (!gc)
        return;
    void* previous = PatchImport(gc, "DisplaySystem.dll", "?process@VisualRain_t@@QAEXMVVector3_t@@VQuaternion_t@@0M@Z",
                                 reinterpret_cast<void*>(&RainProcess));
    if (!previous) {
        RvkLog("weather: Gamecode's import of VisualRain_t::process not found - the game's rain stays");
        g_gaveUp = true;
        return;
    }
    g_process = reinterpret_cast<ProcessFn>(previous);
    g_installed = true;
    RvkLog("weather: VisualRain_t::process hooked (Gamecode %p)", (void*)gc);
}

}  // namespace rvkproxy
