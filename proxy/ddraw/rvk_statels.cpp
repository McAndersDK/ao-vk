// The playfield's placed objects for the renderer's shadows (rvk/statels.cpp): each presented frame, which playfield
// the game is in (N3: n3EngineClient_t::GetPlayfield, its resource id - the statel file's, rdb 1000003); on a change
// the renderer loads its pack, <client>/randy-vk/statels/<id>.bin (ao-assets: python -m aoassets statels). Only
// outdoor playfields (n3Playfield_t::IsGround): indoors the statels belong to rooms and there is no sun.
#include "rvk_backend.h"
#include "rvk_settings.h"

#include <windows.h>

#include <cstdint>
#include <cstdio>
#include <string>

namespace rvkproxy {

namespace {

using GetPlayfieldFn = void*(__cdecl*)();
using GetResourceIdFn = void*(__fastcall*)(const void* self, void* edx, uint32_t* identity);   // (__thiscall, by value)
using IsGroundFn = bool(__fastcall*)(const void* self, void* edx);

GetPlayfieldFn g_getPlayfield = nullptr;
GetResourceIdFn g_getResourceId = nullptr;
IsGroundFn g_isGround = nullptr;
bool g_gaveUp = false;
const void* g_lastPlayfield = reinterpret_cast<const void*>(1);
uint32_t g_lastId = ~0u;
rvk::ThreadedDevice* g_lastDevice = nullptr;

bool Resolve()
{
    if (g_getPlayfield)
        return true;
    HMODULE n3 = GetModuleHandleA("N3.dll");
    if (!n3)
        return false;
    g_getPlayfield = reinterpret_cast<GetPlayfieldFn>(GetProcAddress(n3, "?GetPlayfield@n3EngineClient_t@@SAPAVn3Playfield_t@@XZ"));
    g_getResourceId = reinterpret_cast<GetResourceIdFn>(GetProcAddress(n3, "?GetResourceId@n3Playfield_t@@QBE?AVIdentity_t@@XZ"));
    g_isGround = reinterpret_cast<IsGroundFn>(GetProcAddress(n3, "?IsGround@n3Playfield_t@@QBE_NXZ"));
    if (!g_getPlayfield || !g_getResourceId || !g_isGround) {
        RvkLog("statels: N3.dll's playfield exports not found - placed objects cast only as drawn");
        g_getPlayfield = nullptr;
        g_gaveUp = true;
        return false;
    }
    return true;
}

}  // namespace

void StatelFrame()
{
    rvk::ThreadedDevice* dev = g_rvk.device;
    if (g_gaveUp || !dev || !Resolve())
        return;
    const void* pf = g_getPlayfield();
    uint32_t id = 0;
    bool ground = false;
    if (pf) {
        uint32_t identity[2] = {};
        g_getResourceId(pf, nullptr, identity);
        id = identity[1];
        ground = g_isGround(pf, nullptr);
    }
    if (pf == g_lastPlayfield && id == g_lastId && dev == g_lastDevice)
        return;
    g_lastPlayfield = pf;
    g_lastId = id;
    g_lastDevice = dev;
    dev->SetSkyPlayfield(id);                    // (its climate leans the sky: rvk sky.cpp)
    if (!pf || !ground) {
        if (pf)
            RvkLog("statels: playfield %u is indoors - no placed objects' shadows", id);
        dev->LoadStatels("", 0);
        return;
    }
    // Next to randy-vk.ini: randy-vk\statels\<id>.bin (with mesh-ids.bin and the material maps).
    std::string dir(rvk_settings::IniPath());
    const size_t slash = dir.find_last_of('\\');
    dir = slash == std::string::npos ? std::string() : dir.substr(0, slash + 1);
    char name[32];
    std::snprintf(name, sizeof(name), "%u.bin", id);
    dev->LoadStatels((dir + "randy-vk\\statels\\" + name).c_str(), id);
}

}  // namespace rvkproxy
