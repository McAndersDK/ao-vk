// RVK_TimeOfDay, RVK_TimeSpeed (testing): the game's time of day set to a chosen hour and run at a chosen speed (x the
// game's own; 0 still), so its sky, suns, light and ours with them can be looked at any time, or a dawn watched go by. The game keeps its clock in Gamecode's GameTime_t (one, exported m_pcInstance): the
// seconds into the day as a double at +0x50, the hours in a day at +0x80 (27 on Rubi-Ka), the seconds in an hour at
// +0x8c; SetCurrentRealTime(seconds) sets all of it (its day period too). The server sets the clock now and then
// (GameTime_t::Update); ours, it is set again each presented frame. Let go (-1 and speed 1), the clock goes on from
// where the game's would be by now (its time when taken, at its own speed) until the server's next word.
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <chrono>
#include <cmath>
#include <cstdint>

namespace rvkproxy {

namespace {

using SetRealTimeFn = void(__fastcall*)(void* self, void* edx, float seconds);   // (__thiscall)
using GetRealTimeFn = float(__fastcall*)(const void* self, void* edx);
using SpeedFn = int(__fastcall*)(const void* self, void* edx);

void* const* g_instance = nullptr;              // &GameTime_t::m_pcInstance
SetRealTimeFn g_setRealTime = nullptr;
GetRealTimeFn g_getRealTime = nullptr;
SpeedFn g_speed = nullptr;
bool g_gaveUp = false;
bool g_held = false;
float g_heldAt = 0.0f;                          // the game's own time when first held (seconds into its day)
std::chrono::steady_clock::time_point g_heldSince;
double g_base = 0.0;                            // our clock: its time (seconds into the day) at g_baseSince
std::chrono::steady_clock::time_point g_baseSince;
float g_baseSpeed = 1.0f, g_lastHour = -1.0f;   // ... its speed (RVK_TimeSpeed), the hour last set

bool Resolve()
{
    if (g_setRealTime)
        return true;
    HMODULE gc = GetModuleHandleA("Gamecode.dll");
    if (!gc)
        return false;
    g_instance = reinterpret_cast<void* const*>(GetProcAddress(gc, "?m_pcInstance@GameTime_t@@0PAV1@A"));
    g_getRealTime = reinterpret_cast<GetRealTimeFn>(GetProcAddress(gc, "?GetCurrentRealTime@GameTime_t@@QBEMXZ"));
    g_speed = reinterpret_cast<SpeedFn>(GetProcAddress(gc, "?GetGameSecondsPerSecond@GameTime_t@@QBEHXZ"));
    g_setRealTime = reinterpret_cast<SetRealTimeFn>(GetProcAddress(gc, "?SetCurrentRealTime@GameTime_t@@QAEXM@Z"));
    if (!g_instance || !g_getRealTime || !g_speed || !g_setRealTime) {
        RvkLog("time: GameTime_t's exports not found in Gamecode.dll - RVK_TimeOfDay off");
        g_setRealTime = nullptr;
        g_gaveUp = true;
        return false;
    }
    return true;
}

}  // namespace

// Each presented frame.
void TimeFrame()
{
    if (g_gaveUp)
        return;
    const float hour = rvk_settings::Get("RVK_TimeOfDay");
    const float speed = rvk_settings::Get("RVK_TimeSpeed");
    const bool own = hour >= 0.0f || std::fabs(speed - 1.0f) > 1e-4f;   // (the defaults: the game's own clock)
    if (!own && !g_held)
        return;
    if (!Resolve())
        return;
    const uint8_t* self = static_cast<const uint8_t*>(*g_instance);
    if (!self)
        return;                                 // (not made yet: before the game is entered)
    const int hoursPerDay = *reinterpret_cast<const int*>(self + 0x80);
    const int secondsPerHour = *reinterpret_cast<const int*>(self + 0x8c);
    if (hoursPerDay < 1 || hoursPerDay > 100 || secondsPerHour < 1 || secondsPerHour > 100000) {
        RvkLog("time: GameTime_t not as expected (%d hours a day, %d seconds an hour) - RVK_TimeOfDay off", hoursPerDay,
               secondsPerHour);
        g_gaveUp = true;
        return;
    }
    void* edx = nullptr;
    const double day = double(hoursPerDay) * double(secondsPerHour);
    const auto now = std::chrono::steady_clock::now();
    const double rate = double(g_speed(self, edx));   // (the game's seconds a second)
    if (!own) {                                 // let go: on from where the game's clock would be by now
        const double gone = std::chrono::duration<double>(now - g_heldSince).count();
        const float at = float(std::fmod(double(g_heldAt) + gone * rate, day));
        g_setRealTime(const_cast<uint8_t*>(self), edx, at);
        g_held = false;
        RvkLog("time: the game's own time of day again (%.2f h)", double(at / float(secondsPerHour)));
        return;
    }
    // Ours: from a base time at a base moment, at the game's speed x RVK_TimeSpeed (0: still). A new hour set starts
    // there; a new speed goes on from where the clock is.
    auto current = [&] {
        const double gone = std::chrono::duration<double>(now - g_baseSince).count();
        return std::fmod(std::fmod(g_base + gone * rate * double(g_baseSpeed), day) + day, day);
    };
    if (!g_held) {
        g_heldAt = g_getRealTime(self, edx);
        g_heldSince = now;
        g_held = true;
        g_base = hour >= 0.0f ? double(hour) * secondsPerHour : double(g_heldAt);
        g_baseSince = now;
        g_baseSpeed = speed;
        g_lastHour = hour;
        RvkLog("time: ours from %.2f h at %.2f x the game's speed (%.0f game seconds a second; the game's was %.2f h; %d "
               "hours a day)", g_base / secondsPerHour, double(speed), rate, double(g_heldAt / float(secondsPerHour)),
               hoursPerDay);
    } else if (hour != g_lastHour) {
        g_base = hour >= 0.0f ? double(hour) * secondsPerHour : current();
        g_baseSince = now;
        g_lastHour = hour;
    }
    if (speed != g_baseSpeed) {
        g_base = current();
        g_baseSince = now;
        g_baseSpeed = speed;
    }
    g_setRealTime(const_cast<uint8_t*>(self), edx, float(current()));
}

}  // namespace rvkproxy
