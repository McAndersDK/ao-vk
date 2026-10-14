// ao_vk_plugin.cpp - ao-vk's AOPlugins plugin (ao-vk.dll): the renderer's settings in the options window (F10).
//
// randy31.dll is the renderer and stays independent of AOPlugins: it exports its settings table as a small C
// interface (RvkSettings_* / RvkPresets_*, proxy/ddraw/rvk_settings.h) and saves its settings itself (randy-vk.ini).
// This plugin turns that table into a "Renderer" tab: each setting a DValue (bool, or int - float settings count their
// steps) bound to a widget, changes passed on to the renderer. The preset buttons, and the client's frame rate cap
// (AFCM.dll's Timer_t::m_vMaximumFramerate, kept by AOPlugins in AOPlugins.ini) sit at the top.
//
// Ported from the Renderer tab of the AOReloaded fork (wannabuh/AOReloaded, src/core/renderer_settings.cpp and
// fps_cap.cpp), where it lived inside version.dll.

#include "aop_plugin.h"
#include "view_distance.h"

#include <windows.h>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

const AopHost* g_host;

// ---- randy-vk's settings interface (proxy/ddraw/rvk_settings.h), version 2 ----

struct RvkSettingInfo {
    uint32_t size;
    const char* name;
    const char* label;
    const char* section;
    uint32_t type;              // 0 bool, 1 int, 2 float, 3 choice (an int from `choices`)
    float min, max, step;
    float value, defaultValue;
    const char* parent;         // the bool switching the feature this setting belongs to (null: none)
    const char* choices;        // choice: the allowed values separated by spaces
};
enum : uint32_t { kBool = 0, kInt = 1, kFloat = 2, kChoice = 3 };

uint32_t(__cdecl* g_count)();
int(__cdecl* g_get)(uint32_t, RvkSettingInfo*);
int(__cdecl* g_set)(const char*, float);
uint32_t(__cdecl* g_presetCount)();             // presets: 0 Original, 1 Classic+, 2 Modern, 3 Ultra
const char*(__cdecl* g_presetName)(uint32_t);
int(__cdecl* g_presetApply)(uint32_t);
int(__cdecl* g_presetCurrent)();
bool g_available;
std::string g_missing = "ao-vk's renderer (randy31.dll) isn't installed";   // why there's no renderer tab content

constexpr char kPresetVar[] = "RVK_Preset";     // the preset buttons; kCustom: none matches
constexpr int32_t kCustom = 99;

// The frame rate cap: the client's own limiter divides by it, so never zero.
constexpr char kFpsVar[] = "AOVK_FpsCap";
constexpr int32_t kStockFps = 100, kMinFps = 30, kMaxFps = 500;
float* g_maxFramerate;

bool GetInfo(uint32_t i, RvkSettingInfo& info) {
    std::memset(&info, 0, sizeof(info));
    info.size = sizeof(info);
    return g_get(i, &info) != 0 && info.name;
}

bool Usable(const RvkSettingInfo& s) {
    const size_t len = std::strlen(s.name);
    return len > 0 && len <= 15;                // the game's DValue name limit
}

int32_t ToSteps(const RvkSettingInfo& s, float v) { return static_cast<int32_t>(std::lround(v / s.step)); }

int32_t AsDValue(const RvkSettingInfo& s, float v) {
    if (s.type == kBool) return v != 0.0f ? 1 : 0;
    if (s.type == kFloat) return ToSteps(s, v);
    return static_cast<int32_t>(std::lround(v));
}

int32_t CurrentPreset() {
    const int p = g_presetCount ? g_presetCurrent() : -1;
    return p < 0 ? kCustom : p;
}

bool Resolve() {
    HMODULE randy = GetModuleHandleA("randy31.dll");
    if (!randy) return false;
    auto version = reinterpret_cast<uint32_t(__cdecl*)()>(GetProcAddress(randy, "RvkSettings_Version"));
    g_count = reinterpret_cast<decltype(g_count)>(GetProcAddress(randy, "RvkSettings_Count"));
    g_get = reinterpret_cast<decltype(g_get)>(GetProcAddress(randy, "RvkSettings_Get"));
    g_set = reinterpret_cast<decltype(g_set)>(GetProcAddress(randy, "RvkSettings_Set"));
    if (!version || !g_count || !g_get || !g_set) {
        g_missing = "randy31.dll is the game's own renderer, not ao-vk's";
        return false;
    }
    if (version() < 2) {
        g_missing = "ao-vk's renderer is too old for this plugin (settings interface 1, 2 needed)";
        return false;
    }
    g_presetCount = reinterpret_cast<decltype(g_presetCount)>(GetProcAddress(randy, "RvkPresets_Count"));
    g_presetName = reinterpret_cast<decltype(g_presetName)>(GetProcAddress(randy, "RvkPresets_Name"));
    g_presetApply = reinterpret_cast<decltype(g_presetApply)>(GetProcAddress(randy, "RvkPresets_Apply"));
    g_presetCurrent = reinterpret_cast<decltype(g_presetCurrent)>(GetProcAddress(randy, "RvkPresets_Current"));
    if (!g_presetCount || !g_presetName || !g_presetApply || !g_presetCurrent) g_presetCount = nullptr;
    return true;
}

void AddSetting(const char* name, uint32_t type, int32_t value, int32_t min, int32_t max, uint32_t flags) {
    AopSetting s = {sizeof(AopSetting), name, type, value, min, max, flags};
    g_host->addSetting(g_host, &s);   // a refusal is in the log
}

// Each renderer setting a DValue. The renderer keeps them (randy-vk.ini): AOPlugins doesn't save them.
void RegisterRendererSettings() {
    const uint32_t count = g_count();
    uint32_t added = 0;
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || !Usable(s)) continue;
        if (s.type == kBool)
            AddSetting(s.name, AOP_SETTING_BOOL, s.value != 0.0f, 0, 1, AOP_SETTING_NOT_SAVED);
        else if (s.type == kFloat)
            AddSetting(s.name, AOP_SETTING_INT, ToSteps(s, s.value), ToSteps(s, s.min), ToSteps(s, s.max),
                       AOP_SETTING_NOT_SAVED);
        else
            AddSetting(s.name, AOP_SETTING_INT, static_cast<int32_t>(std::lround(s.value)),
                       static_cast<int32_t>(s.min), static_cast<int32_t>(s.max), AOP_SETTING_NOT_SAVED);
        ++added;
    }
    if (g_presetCount) AddSetting(kPresetVar, AOP_SETTING_INT, CurrentPreset(), 0, kCustom, AOP_SETTING_NOT_SAVED);
    g_host->log(g_host, "renderer settings: %u (presets: %s)", added, g_presetCount ? "yes" : "no");
}

// Every renderer setting's DValue (and the preset buttons) to the renderer's values - a preset changes many at once.
// setSetting: the options window follows, without reporting the change back here.
void SyncAll() {
    const uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (GetInfo(i, s) && Usable(s)) g_host->setSetting(g_host, s.name, AsDValue(s, s.value));
    }
    if (g_presetCount) g_host->setSetting(g_host, kPresetVar, CurrentPreset());
}

void TryRenderer() {
    if (g_available) return;
    g_available = Resolve();
    if (g_available) RegisterRendererSettings();
    else g_host->log(g_host, "no renderer settings: %s", g_missing.c_str());
}

// ---- The frame rate cap ----

void ApplyFps(int32_t fps) {
    if (!g_maxFramerate) return;
    fps = fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps;
    *g_maxFramerate = static_cast<float>(fps);   // an aligned 4-byte store: the game thread never sees half of it
    g_host->log(g_host, "maximum frame rate %d", fps);
}

// AFCM.dll is a static import of AnarchyOnline.exe: loaded by gameReady.
void InitFpsCap() {
    HMODULE afcm = GetModuleHandleA("AFCM.dll");
    g_maxFramerate = afcm ? reinterpret_cast<float*>(GetProcAddress(afcm, "?m_vMaximumFramerate@Timer_t@@2MA")) : nullptr;
    if (!g_maxFramerate) {
        g_host->log(g_host, "no frame rate cap: AFCM.dll's Timer_t::m_vMaximumFramerate not found");
        return;
    }
    int32_t fps = kStockFps;
    g_host->getSetting(g_host, kFpsVar, &fps);
    g_host->log(g_host, "client frame rate cap was %.0f", static_cast<double>(*g_maxFramerate));
    ApplyFps(fps);
}


// ---- The Renderer tab ----

void Out(AopBuffer* out, const std::string& text) { out->append(out, text.data(), static_cast<uint32_t>(text.size())); }

std::string Escape(const char* text) {
    std::string r;
    for (const char* p = text ? text : ""; *p; ++p) {
        switch (*p) {
        case '&': r += "&amp;"; break;
        case '<': r += "&lt;"; break;
        case '>': r += "&gt;"; break;
        case '"': r += "&quot;"; break;
        default: r += *p;
        }
    }
    return r;
}

bool Is(const char* a, const char* b) { return a && b && std::strcmp(a, b) == 0; }

// A setting in the "Experimental" section: shown in its own block under the switches, off by default (the defaults
// and the section are randy-vk's).
bool IsExperimental(const RvkSettingInfo& s) { return Is(s.section, "Experimental"); }

// The settings whose `parent` is `p`, of the requested kind (bool switches or values).
std::vector<const RvkSettingInfo*> ChildrenOf(const std::vector<RvkSettingInfo>& all, const RvkSettingInfo& p,
                                              bool bools) {
    std::vector<const RvkSettingInfo*> list;
    for (const RvkSettingInfo& c : all)
        if (Is(c.parent, p.name) && (c.type == kBool) == bools) list.push_back(&c);
    return list;
}

void Heading(std::string& x, const char* text, int top, const char* font = "HUGE") {
    char attrs[96];
    std::snprintf(attrs, sizeof(attrs), "\" font=\"%s\" layout_borders=\"Rect(0,%d,0,3)\" />\n", font, top);
    x += "\n        <TextView value=\"" + Escape(text) + attrs;
}

void Note(std::string& x, const char* text) {
    x += "        <TextView value=\"" + Escape(text) + "\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" />\n";
}

void CheckBox(std::string& x, const RvkSettingInfo& s, int indent) {
    char attrs[128];
    std::snprintf(attrs, sizeof(attrs),
                  "\" font=\"LARGE\" layout_borders=\"Rect(%d,0,0,0)\" opt_type=\"variant\" opt_variable=\"", indent);
    x += "        <OptionCheckBox label=\"" + Escape(s.label) + attrs + s.name + "\"/>\n";
}

// A slider (int: the value; float: steps, shown scaled back) or, for a choice, radio buttons of its values.
void Value(std::string& x, const RvkSettingInfo& s) {
    if (s.type == kChoice && s.choices) {
        x += "        <OptionRadioButtonGroup label=\"" + Escape(s.label) +
             ":\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"" + s.name +
             "\">\n";
        for (const char* c = s.choices; *c;) {
            while (*c == ' ') ++c;
            const char* e = c;
            while (*e && *e != ' ') ++e;
            if (e == c) break;
            const std::string value(c, e);
            x += "          <RadioButton label=\"" + Escape(value.c_str()) + "\" value=\"" + Escape(value.c_str()) +
                 "\"/>\n";
            c = e;
        }
        x += "        </OptionRadioButtonGroup>\n";
        return;
    }
    char attrs[192];
    if (s.type == kFloat)
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.2f&lt;/font&gt;\" value_scale=\"%g\"/>\n",
                      static_cast<double>(s.step));
    else
        std::snprintf(attrs, sizeof(attrs),
                      "\" font=\"LARGE\" value_fmt=\"&lt;font color=#70C4D0&gt;%%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n");
    x += "        <OptionSlider label=\"" + Escape(s.label) +
         ":\" layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"" + s.name + attrs;
}

void FrameRate(std::string& x, int top) {
    Heading(x, "Frame rate", top);
    x += std::string("        <OptionSlider label=\"Maximum frame rate (default: 100):\" font=\"LARGE\""
                     " layout_borders=\"Rect(10,0,0,3)\" opt_type=\"variant\" opt_variable=\"") +
         kFpsVar + "\" value_fmt=\"&lt;font color=#70C4D0&gt;%.0f&lt;/font&gt;\" value_scale=\"1\"/>\n";
}

void Presets(std::string& x) {
    Heading(x, "Preset", 0);
    x += std::string("        <OptionRadioButtonGroup label=\"Look:\" font=\"LARGE\" layout_borders=\"Rect(10,0,0,3)\""
                     " opt_type=\"variant\" opt_variable=\"") + kPresetVar + "\">\n";
    const uint32_t n = g_presetCount();
    for (uint32_t i = 0; i < n; ++i)
        x += "          <RadioButton label=\"" + Escape(g_presetName(i)) + "\" value=\"" + std::to_string(i) + "\"/>\n";
    x += "          <RadioButton label=\"Custom\" value=\"" + std::to_string(kCustom) + "\"/>\n";
    x += "        </OptionRadioButtonGroup>\n";
    Note(x, "To keep a setup of your own, back up randy-vk.ini.");
}

// The experimental features: the switches first (so they sit right under the other enhancements), then their values
// under each feature.
void Experimental(std::string& x, const std::vector<RvkSettingInfo>& all) {
    bool any = false;
    for (const RvkSettingInfo& s : all) any |= !s.parent && IsExperimental(s);
    if (!any) return;
    Heading(x, "Experimental", 6);
    Note(x, "New and unfinished features. Off by default; they may be unstable or change.");
    for (const RvkSettingInfo& p : all) {
        if (p.parent || !IsExperimental(p)) continue;
        CheckBox(x, p, 10);
        for (const RvkSettingInfo* c : ChildrenOf(all, p, true)) CheckBox(x, *c, 30);
    }
    for (const RvkSettingInfo& p : all) {
        if (p.parent || !IsExperimental(p)) continue;
        const std::vector<const RvkSettingInfo*> values = ChildrenOf(all, p, false);
        if (values.empty()) continue;
        Heading(x, p.label, 8);
        for (const RvkSettingInfo* v : values) Value(x, *v);
    }
}

// Every on / off option at the top (by section, a feature's own options indented under it), the experimental ones
// in a block of their own, then the values (sliders, choices) in blocks headed by their feature or section.
void __cdecl WriteTab(void* /*user*/, AopBuffer* out) {
    std::string x;
    x += "        <TextView value=\"ao-vk renderer\" font=\"HUGE\" layout_borders=\"Rect(0,0,0,5)\" />\n";
    if (!g_available) {
        x += "        <TextView value=\"" + Escape(g_missing.c_str()) +
             ". The frame rate cap works without it.\" font=\"LARGE\" layout_borders=\"Rect(0,0,0,10)\" />\n";
        FrameRate(x, 0);
        aovk::viewdist::WriteTab(x);
        Out(out, x);
        return;
    }
    x += "        <TextView value=\"Changes apply at once. Settings are saved to randy-vk.ini.\" font=\"LARGE\""
         " layout_borders=\"Rect(0,0,0,10)\" />\n";
    if (g_presetCount) Presets(x);
    FrameRate(x, g_presetCount ? 12 : 0);
    aovk::viewdist::WriteTab(x);

    std::vector<RvkSettingInfo> all;
    const uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (GetInfo(i, s) && Usable(s)) all.push_back(s);
    }

    Heading(x, "Options", 16);
    const char* section = "";
    for (const RvkSettingInfo& s : all) {
        if (s.type != kBool || s.parent || IsExperimental(s)) continue;
        if (!Is(section, s.section)) {
            section = s.section;
            Heading(x, section, 6);
        }
        CheckBox(x, s, 10);
        for (const RvkSettingInfo* c : ChildrenOf(all, s, true)) CheckBox(x, *c, 30);
    }
    Experimental(x, all);

    Heading(x, "Adjustments", 16);
    const char* heading = "";
    for (const RvkSettingInfo& s : all) {
        if (s.parent || IsExperimental(s)) continue;
        if (s.type == kBool) {
            const std::vector<const RvkSettingInfo*> values = ChildrenOf(all, s, false);
            if (values.empty()) continue;
            Heading(x, s.label, 8);
            heading = s.label;
            for (const RvkSettingInfo* v : values) Value(x, *v);
        } else {
            if (!Is(heading, s.section)) Heading(x, s.section, 8);
            heading = s.section;
            Value(x, s);
        }
    }
    Out(out, x);
}

// ---- Events ----

void __cdecl GameReady() {
    aovk::viewdist::GameReady();
    InitFpsCap();
    TryRenderer();
}

// The renderer may be loaded after Utils.dll: one more look. (If it is only found now, the tab the game has already
// read has no settings; they are there from the next start.)
void __cdecl WorldReady() {
    aovk::viewdist::WorldReady();
    if (!g_available) TryRenderer();
}

void __cdecl GameFrame() { aovk::viewdist::GameFrame(); }

void __cdecl DValueChanged(const char* name, const AopValue* value) { aovk::viewdist::DValueChanged(name, value); }

void __cdecl SettingChanged(const char* name, int32_t value) {
    if (aovk::viewdist::SettingChanged(name, value)) return;
    if (std::strcmp(name, kFpsVar) == 0) {
        ApplyFps(value);
        return;
    }
    if (!g_available) return;
    if (g_presetCount && std::strcmp(name, kPresetVar) == 0) {
        if (value != kCustom && g_presetApply(static_cast<uint32_t>(value)))
            g_host->log(g_host, "preset %s", g_presetName(static_cast<uint32_t>(value)));
        SyncAll();                              // the window shows the preset's values
        return;
    }
    const uint32_t count = g_count();
    for (uint32_t i = 0; i < count; ++i) {
        RvkSettingInfo s;
        if (!GetInfo(i, s) || std::strcmp(s.name, name) != 0) continue;
        float setting = s.type == kFloat ? static_cast<float>(value) * s.step : static_cast<float>(value);
        if (s.type == kBool) setting = value ? 1.0f : 0.0f;
        g_set(name, setting);                   // the renderer applies and saves it
        g_host->log(g_host, "%s = %g", name, static_cast<double>(setting));
        if (g_presetCount) g_host->setSetting(g_host, kPresetVar, CurrentPreset());   // by hand: Custom, likely
        return;
    }
}

const AopPluginInfo kInfo = {
    sizeof(AopPluginInfo), AOP_API_VERSION,
    "ao-vk", "ao-vk", AOP_VERSION(1, 1, 0),
    AOP_PLUGIN_EXCLUSIVE,   // a project: the renderer changes how the whole game looks
    nullptr, 0,
};

const AopPlugin kPlugin = {
    AOP_API_VERSION, sizeof(AopPlugin), &GameReady, &WorldReady, &SettingChanged, &DValueChanged, nullptr, &GameFrame,
};

}  // namespace

AOP_EXPORT const AopPluginInfo* __cdecl AopPluginQuery(void) {
    return &kInfo;
}

AOP_EXPORT const AopPlugin* __cdecl AopPluginAttach(const AopHost* host) {
    // API 2: patches, DValue ranges, the game frame (view distance).
    if (host->version < 2 || host->size < sizeof(AopHost)) {
        host->log(host, "AOPlugins is too old for this plugin (plugin API %u, 2 needed)", host->version);
        return nullptr;
    }
    g_host = host;
    aovk::viewdist::Attach(host);
    // The cap is AOPlugins' to keep (AOPlugins.ini [ao-vk]); the renderer's settings come in gameReady, once
    // randy31.dll can be asked.
    // The AOReloaded fork kept the cap as AOR_FpsCap in AOReloaded.ini: its value is the default here, once.
    const int32_t fps = aovk::viewdist::AOReloadedIni(host, "AOR_FpsCap", kStockFps);
    AddSetting(kFpsVar, AOP_SETTING_INT, fps < kMinFps ? kMinFps : fps > kMaxFps ? kMaxFps : fps, kMinFps, kMaxFps, 0);
    // "Renderer" also replaces a static copy of the tab in a GUI skin (PRK-VK's Root.xml has one).
    if (!host->addOptionsTab(host, "Renderer", &WriteTab, nullptr)) return nullptr;
    return &kPlugin;
}
