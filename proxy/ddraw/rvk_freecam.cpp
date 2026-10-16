// A free camera for debugging (Ctrl+Shift+C): flying through the scene with the mouse and WASD while the character
// stays put.
//
// - Where it is: FreeCamWorld, from the native viewport's scene update (proxy/native/viewport.cpp Process), puts it in
//   the game's own camera before its view and frustum are made - what the game draws and culls follows it. It starts
//   where the game's camera is. (Without the native scene: the renderer's own override, Device::SetFreeCam - the view
//   only, the game culling from its own camera.)
// - Its controls (FreeCamFrame, each presented frame, while the game's window has the focus): W / S forward and back,
//   A / D sideways, E / Q up and down, Shift four times as fast, the mouse wheel faster or slower; the mouse looks about
//   (the cursor hidden and kept in the window's middle).
// - The game kept from the input meanwhile: DirectInput (DisplaySystem.dll's keyboard and mouse) reads nothing - the
//   device method table every DirectInput device of the process shares is found by making a device of our own, and
//   its GetDeviceState / GetDeviceData give an idle state; and the game's window gets no key, mouse or raw input
//   messages (subclassed while on). Our own keys come from GetAsyncKeyState, which neither touches.
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>

#include "rvk_backend.h"

namespace rvkproxy {

namespace {

struct State {
    bool on = false, placed = false;            // on; placed: started from the game's camera
    float pos[3] = {}, yaw = 0.0f, pitch = 0.0f, speed = 8.0f;
    LARGE_INTEGER last{};
    uint64_t nativeFrame = 0;                   // the last present the native scene took it (FreeCamWorld)
    uint64_t onSince = 0;                        // the present it was turned on at
    bool deviceOverride = false;                 // the renderer's own override in use (no native scene)
    float wheel = 0.0f;                          // wheel notches since the last frame (the window procedure)
} g;
std::atomic<bool> g_blocking{false};
uint64_t g_presents = 0;

// ---------------------------------------------------------------- DirectInput: an idle state while blocking
using GetStateFn = HRESULT(__stdcall*)(void*, DWORD, void*);
using GetDataFn = HRESULT(__stdcall*)(void*, DWORD, void*, DWORD*, DWORD);
struct Patched { void** vtable; GetStateFn state; GetDataFn data; };
Patched g_patched[4];
int g_patchedCount = 0;
bool g_dinputTried = false;

const Patched* PatchedFor(void* self)
{
    void** vtable = *static_cast<void***>(self);
    for (int i = 0; i < g_patchedCount; ++i)
        if (g_patched[i].vtable == vtable) return &g_patched[i];
    return nullptr;
}

HRESULT __stdcall HookGetDeviceState(void* self, DWORD bytes, void* data)
{
    const Patched* p = PatchedFor(self);
    if (!p) return E_FAIL;
    HRESULT r = p->state(self, bytes, data);
    if (g_blocking && SUCCEEDED(r) && data) std::memset(data, 0, bytes);   // (nothing pressed, no movement)
    return r;
}

// Whether a device is a mouse (its type: DIDEVICEINSTANCEA.dwDevType's low byte 2; IDirectInputDevice slot 15
// GetDeviceInfo), asked once each.
bool IsMouse(void* self)
{
    static void* known[8];
    static bool mouse[8];
    static int count = 0;
    for (int i = 0; i < count; ++i)
        if (known[i] == self) return mouse[i];
    struct { DWORD size; GUID instance, product; DWORD type; char names[2 * 260]; GUID ff; WORD page, usage; } info{};
    info.size = sizeof(info);
    using GetInfo = HRESULT(__stdcall*)(void*, void*);
    const bool m = SUCCEEDED(reinterpret_cast<GetInfo>((*static_cast<void***>(self))[15])(self, &info)) &&
                   (info.type & 0xFF) == 2;
    if (count < 8) { known[count] = self; mouse[count++] = m; }
    return m;
}

HRESULT __stdcall HookGetDeviceData(void* self, DWORD objectBytes, void* data, DWORD* count, DWORD flags)
{
    const Patched* p = PatchedFor(self);
    if (!p) return E_FAIL;
    HRESULT r = p->data(self, objectBytes, data, count, flags);   // (drained as usual)
    if (g_blocking && SUCCEEDED(r) && count) {
        // Only the releases kept (a key, a button let go): held when it went on (Ctrl+Shift+C itself), the game would
        // think them held still. (DIDEVICEOBJECTDATA: offset, data - bit 7 down -, ...; a mouse's axes at offsets
        // 0 / 4 / 8: dropped.)
        if (!data || objectBytes < 8) {
            *count = 0;
        } else {
            const bool mouse = IsMouse(self);
            uint8_t* d = static_cast<uint8_t*>(data);
            DWORD kept = 0;
            for (DWORD i = 0; i < *count; ++i) {
                const DWORD* e = reinterpret_cast<const DWORD*>(d + size_t(i) * objectBytes);
                if ((e[1] & 0x80) == 0 && !(mouse && e[0] < 12)) {
                    if (kept != i) std::memmove(d + size_t(kept) * objectBytes, e, objectBytes);
                    ++kept;
                }
            }
            *count = kept;
        }
    }
    return r;
}

void PatchVtable(void** vtable)
{
    if (!vtable || g_patchedCount >= 4) return;
    for (int i = 0; i < g_patchedCount; ++i)
        if (g_patched[i].vtable == vtable) return;
    DWORD old;
    if (!VirtualProtect(&vtable[9], sizeof(void*) * 2, PAGE_EXECUTE_READWRITE, &old)) return;
    Patched& p = g_patched[g_patchedCount++];
    p.vtable = vtable;
    p.state = reinterpret_cast<GetStateFn>(vtable[9]);     // IDirectInputDevice: 9 GetDeviceState, 10 GetDeviceData
    p.data = reinterpret_cast<GetDataFn>(vtable[10]);
    vtable[9] = reinterpret_cast<void*>(&HookGetDeviceState);
    vtable[10] = reinterpret_cast<void*>(&HookGetDeviceData);
    VirtualProtect(&vtable[9], sizeof(void*) * 2, old, &old);
}

// The keyboard's and mouse's method tables: from devices of our own (DirectInputCreateA, version 7, as the game's).
void PatchDirectInput()
{
    if (g_dinputTried) return;
    g_dinputTried = true;
    HMODULE dinput = LoadLibraryA("dinput.dll");
    auto create = dinput ? reinterpret_cast<HRESULT(__stdcall*)(HINSTANCE, DWORD, void**, void*)>(
                               GetProcAddress(dinput, "DirectInputCreateA")) : nullptr;
    void* di = nullptr;
    if (!create || FAILED(create(GetModuleHandleA(nullptr), 0x0700, &di, nullptr)) || !di) {
        RvkLog("free camera: no DirectInput - the game's own input not held back");
        return;
    }
    static const GUID kKeyboard = {0x6F1D2B61, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    static const GUID kMouse = {0x6F1D2B60, 0xD5A0, 0x11CF, {0xBF, 0xC7, 0x44, 0x45, 0x53, 0x54, 0x00, 0x00}};
    using CreateDevice = HRESULT(__stdcall*)(void*, const GUID&, void**, void*);
    using Release = ULONG(__stdcall*)(void*);
    void** diVtable = *static_cast<void***>(di);
    for (const GUID* guid : {&kKeyboard, &kMouse}) {
        void* dev = nullptr;
        if (SUCCEEDED(reinterpret_cast<CreateDevice>(diVtable[3])(di, *guid, &dev, nullptr)) && dev) {
            PatchVtable(*static_cast<void***>(dev));
            reinterpret_cast<Release>((*static_cast<void***>(dev))[2])(dev);
        }
    }
    reinterpret_cast<Release>(diVtable[2])(di);
    RvkLog("free camera: DirectInput's keyboard and mouse held back while on (%d method tables)", g_patchedCount);
}

// ---------------------------------------------------------------- the window: no input messages while blocking
HWND g_window = nullptr;
WNDPROC g_windowProc = nullptr;

LRESULT CALLBACK FreeCamWindowProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp)
{
    if (g_blocking) {
        if (msg == WM_MOUSEWHEEL) {
            g.wheel += float(GET_WHEEL_DELTA_WPARAM(wp)) / float(WHEEL_DELTA);
            return 0;
        }
        if (msg == WM_INPUT || msg == WM_SETCURSOR)
            return DefWindowProcA(hwnd, msg, wp, lp);
        // (Releases go through: a key or button held when it went on - Ctrl+Shift+C itself - must not stay held.)
        const bool release = msg == WM_KEYUP || msg == WM_SYSKEYUP || msg == WM_LBUTTONUP || msg == WM_RBUTTONUP ||
                             msg == WM_MBUTTONUP || msg == WM_XBUTTONUP;
        if (!release && ((msg >= WM_KEYFIRST && msg <= WM_KEYLAST) || (msg >= WM_MOUSEFIRST && msg <= WM_MOUSELAST)))
            return 0;
    }
    return CallWindowProcA(g_windowProc, hwnd, msg, wp, lp);
}

void Subclass(HWND window)
{
    if (!window || g_window == window) return;
    g_window = window;
    g_windowProc = reinterpret_cast<WNDPROC>(
        SetWindowLongPtrA(window, GWLP_WNDPROC, reinterpret_cast<LONG_PTR>(&FreeCamWindowProc)));
}

bool Focused()
{
    DWORD pid = 0;
    GetWindowThreadProcessId(GetForegroundWindow(), &pid);
    return pid == GetCurrentProcessId();
}

}  // namespace

bool FreeCamOn() { return g.on; }

void FreeCamToggle(HWND window)
{
    g.on = !g.on;
    if (g.on) {
        PatchDirectInput();
        Subclass(window);
        g.placed = false;
        g.onSince = g_presents;
        g.last.QuadPart = 0;
        g.wheel = 0.0f;
        ShowCursor(FALSE);
    } else {
        ShowCursor(TRUE);
    }
    g_blocking = g.on;                          // (the subclass stays, inert while off: another may have chained on)
    RvkLog("free camera %s (mouse: look; W/S/A/D: move, E/Q: up/down, Shift: faster, wheel: speed)", g.on ? "on" : "off");
}

// The native scene's camera, once a frame before its view and frustum: false while off; else its world matrix the
// free camera's (D3D rows: right, up, forward, position) - started, the first time, where `game` (its own) is.
bool FreeCamWorld(const float game[16], float out[16])
{
    if (!g.on) return false;
    g.nativeFrame = g_presents;
    if (!g.placed) {
        g.pos[0] = game[12];
        g.pos[1] = game[13];
        g.pos[2] = game[14];
        const float fl = std::sqrt(game[8] * game[8] + game[9] * game[9] + game[10] * game[10]);
        if (fl < 1e-6f) return false;
        g.yaw = std::atan2(game[8], game[10]);
        g.pitch = std::asin(std::clamp(game[9] / fl, -1.0f, 1.0f));
        g.placed = true;
    }
    const float cy = std::cos(g.yaw), sy = std::sin(g.yaw), cp = std::cos(g.pitch), sp = std::sin(g.pitch);
    const float f[3] = {cp * sy, sp, cp * cy}, r[3] = {cy, 0.0f, -sy};
    const float u[3] = {f[1] * r[2] - f[2] * r[1], f[2] * r[0] - f[0] * r[2], f[0] * r[1] - f[1] * r[0]};
    std::memset(out, 0, 64);
    for (int i = 0; i < 3; ++i) {
        out[i] = r[i];
        out[4 + i] = u[i];
        out[8 + i] = f[i];
        out[12 + i] = g.pos[i];
    }
    out[15] = 1.0f;
    return true;
}

// Each presented frame: the keys and the mouse into the free camera (placed by the native scene; without it, the
// renderer's own override).
void FreeCamFrame(rvk::ThreadedDevice* device, HWND window)
{
    ++g_presents;
    static bool wasOn = false;
    if (!g.on && !wasOn) return;
    LARGE_INTEGER now, freq;
    QueryPerformanceCounter(&now);
    QueryPerformanceFrequency(&freq);
    const float dt = g.last.QuadPart ? float(double(now.QuadPart - g.last.QuadPart) / double(freq.QuadPart)) : 0.0f;
    g.last = now;
    float move[3] = {}, look[2] = {};
    if (g.on && Focused()) {
        auto key = [](int k) { return (GetAsyncKeyState(k) & 0x8000) != 0; };
        move[0] = (key('D') ? 1.0f : 0.0f) - (key('A') ? 1.0f : 0.0f);
        move[1] = (key('E') ? 1.0f : 0.0f) - (key('Q') ? 1.0f : 0.0f);
        move[2] = (key('W') ? 1.0f : 0.0f) - (key('S') ? 1.0f : 0.0f);
        if (g.wheel != 0.0f) {
            g.speed = std::clamp(g.speed * std::pow(1.25f, g.wheel), 0.5f, 400.0f);
            g.wheel = 0.0f;
        }
        // The mouse: how far from the window's middle since last frame, and back there.
        RECT rc;
        if (window && GetClientRect(window, &rc)) {
            POINT mid{(rc.left + rc.right) / 2, (rc.top + rc.bottom) / 2}, cursor;
            ClientToScreen(window, &mid);
            if (GetCursorPos(&cursor) && dt > 0.0f) {
                look[0] = float(cursor.x - mid.x) * 0.0035f;
                look[1] = -float(cursor.y - mid.y) * 0.0035f;
            }
            SetCursorPos(mid.x, mid.y);
        }
    }
    const float step = g.speed * (GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 4.0f : 1.0f) * std::clamp(dt, 0.0f, 0.1f);
    // (The native scene takes it within a frame or two of turning on; only without one, the renderer's override.)
    const bool native = g.nativeFrame + 2 >= g_presents && g.placed;
    const bool fallback = g.on && !native && g_presents > g.onSince + 3;
    if (!fallback) {
        if (g.on) {
            g.yaw += look[0];
            g.pitch = std::clamp(g.pitch + look[1], -1.55f, 1.55f);
            const float cy = std::cos(g.yaw), sy = std::sin(g.yaw), cp = std::cos(g.pitch), sp = std::sin(g.pitch);
            const float f[3] = {cp * sy, sp, cp * cy}, r[3] = {cy, 0.0f, -sy};
            for (int i = 0; i < 3; ++i)
                g.pos[i] += (r[i] * move[0] + (i == 1 ? move[1] : 0.0f) + f[i] * move[2]) * step;
        }
        if (g.deviceOverride) {                  // (the renderer's override off, if it was the one in use)
            rvk::Device::FreeCamInput in;
            device->SetFreeCam(in);
            g.deviceOverride = false;
        }
    } else {                                     // no native scene: the renderer's own override (the view only)
        g.deviceOverride = true;
        rvk::Device::FreeCamInput in;
        in.on = true;
        std::memcpy(in.move, move, sizeof(move));
        std::memcpy(in.look, look, sizeof(look));
        in.speed = g.speed * (GetAsyncKeyState(VK_SHIFT) & 0x8000 ? 4.0f : 1.0f);
        in.dt = std::clamp(dt, 0.0f, 0.1f);
        device->SetFreeCam(in);
    }
    wasOn = g.on;
}

}  // namespace rvkproxy
