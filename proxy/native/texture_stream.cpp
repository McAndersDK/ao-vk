// TextureStreamCreator's bitmap path (randy-vk.ini [Native] Device=on): CreateTexture(LBitmap_t*, name) scales the
// bitmap by the creator's divisor, picks a D3DX surface format from the quality / compression settings, makes the
// surface and uploads the pixels (render_t::D3DXCreateTexture / D3DXLoadTextureFromMemory). EnableCompression is the
// static switch it uses. The ctors, Archive / Instantiate, the PositionIO path and the format helpers are separate.
#include "native/texture_stream.h"

#include "native/lbitmap.h"
#include "native/orig_api.gen.h"
#include "native/serialize.h"

#include <ddraw.h>

#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <list>
#include <mutex>
#include <unordered_map>
#include <vector>

namespace rnative::texturestream {

namespace {

HMODULE g_orig;

template <typename T>
T& G(uint32_t rva) { return *reinterpret_cast<T*>(reinterpret_cast<uint8_t*>(g_orig) + rva); }
template <typename F>
F Internal(uint32_t rva) { return reinterpret_cast<F>(AddressFor(rva)); }

// TextureStreamCreator's statics and the renderer's compression / format-support flags.
constexpr uint32_t kQuality = 0xB6608, kCompression = 0xB660C, kSupport = 0xB6610;   // s_nTextureQuality, s_bCompression, the format cache
constexpr uint32_t kDxt1 = 0x17D330, kSquared = 0x17D31D, kRender = 0x16BED0;
constexpr uint32_t kVtable = 0x8AA44;                 // TextureStreamCreator::vftable

// VS2010 std::string (library): assign(const char*), find(const char*, pos) and _Tidy.
constexpr uint32_t kAssign = 0x1468C, kFind = 0x19826, kTidy = 0x11E82, kFormat = 0x1994C;
constexpr uint32_t kAssignOwn = 0x17B42;              // the std::string assign the ctors use
constexpr uint32_t kStlGrow = 0x1798F, kStlResize = 0x17969;
// FUN_100416f6 (compression supported) and FUN_10041e93 (the format-support flag).
constexpr uint32_t kCompressionSupported = 0x416F6, kFormatSupport = 0x41E93;

void SetVtable(Creator* self) { self->vtable = reinterpret_cast<uint8_t*>(g_orig) + kVtable; }

// D3DX's output, remembered. Loading a bitmap into a texture (render_t::D3DXLoadTextureFromMemory) goes through D3DX's
// conversion core, a filtered resample with a call per source pixel, for level 0 and again for every mip level: about
// 0.4 ms for a 128 x 128 tile here, and the planet map makes ~110 of them each time it opens (a 45-70 ms hitch).
// The same bitmap converted the same way gives the same levels, so a texture's levels are kept (by a hash of the
// bitmap's pixels and everything else that goes into the conversion) and copied in when that bitmap comes again.
// Exact by construction: what is copied is what D3DX made. Uncompressed formats only; 64 MB, least recently used out.
// Textures up to 128 x 128 (the map's tiles, interface pieces) are kept the first time; bigger ones up to 512 x 512
// only when converted a second time (the options window's, opened again and again) - a zone's textures, made once,
// filled it and pushed the tiles out.
// RANDYVK_TEXTURE_CACHE=0 turns it off (A/B).
namespace cache {

struct Level {
    uint32_t rowBytes, rows;
    std::vector<uint8_t> data;
};
struct Entry {
    uint64_t key;
    std::vector<Level> levels;
    size_t bytes;
};

constexpr size_t kBytes = size_t(64) << 20;
std::mutex g_mutex;
std::list<Entry> g_lru;                                   // front = most recently used
std::unordered_map<uint64_t, uint8_t> g_seen;             // bigger textures made once (their keys only; cleared at 64k)
std::unordered_map<uint64_t, std::list<Entry>::iterator> g_index;
size_t g_bytes;
uint32_t g_hits, g_misses;

bool Enabled()
{
    static const bool on = [] {
        char v[8] = "";
        return !(GetEnvironmentVariableA("RANDYVK_TEXTURE_CACHE", v, sizeof(v)) && v[0] == '0');
    }();
    return on;
}

uint64_t Key(const lbitmap::Bitmap* b, int32_t format, uint32_t levels, uint32_t w, uint32_t h, int32_t filter)
{
    const size_t bytes = size_t(b->width) * b->height * size_t(b->bitsPerPixel / 8);
    uint64_t k = 0xcbf29ce484222325ull;
    auto mix = [&k](uint64_t v) { k = (k ^ v) * 0x9E3779B97F4A7C15ull; k ^= k >> 29; };
    for (uint64_t v : {uint64_t(b->width), uint64_t(b->height), uint64_t(b->bitsPerPixel), uint64_t(uint32_t(format)),
                       uint64_t(levels), uint64_t(w), uint64_t(h), uint64_t(uint32_t(filter))})
        mix(v);
    const uint8_t* p = b->data;
    size_t i = 0;
    for (; i + 8 <= bytes; i += 8) {
        uint64_t v;
        std::memcpy(&v, p + i, 8);
        mix(v);
    }
    uint64_t tail = 0;
    std::memcpy(&tail, p + i, bytes - i);
    mix(tail ^ (uint64_t(bytes) << 32));
    return k;
}

// Calls f(level surface) for the top level and each mip level below it (as D3DX walks them).
template <typename F>
void ForEachLevel(IDirectDrawSurface7* top, F f)
{
    IDirectDrawSurface7* s = top;
    s->AddRef();
    while (s) {
        const bool more = f(s);
        IDirectDrawSurface7* next = nullptr;
        DDSCAPS2 caps{DDSCAPS_TEXTURE | DDSCAPS_MIPMAP, 0, 0, {0}};
        if (!more || FAILED(s->GetAttachedSurface(&caps, &next))) next = nullptr;
        s->Release();
        s = next;
    }
}

uint32_t RowBytes(const DDSURFACEDESC2& sd) { return sd.dwWidth * (sd.ddpfPixelFormat.dwRGBBitCount / 8); }

bool Uncompressed(const DDSURFACEDESC2& sd)
{
    return !(sd.ddpfPixelFormat.dwFlags & DDPF_FOURCC) && sd.ddpfPixelFormat.dwRGBBitCount >= 8;
}

// The texture's levels as D3DX left them.
bool Read(IDirectDrawSurface7* top, Entry* out)
{
    bool ok = true;
    ForEachLevel(top, [&](IDirectDrawSurface7* s) {
        DDSURFACEDESC2 sd{};
        sd.dwSize = sizeof(sd);
        if (FAILED(s->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_READONLY, nullptr))) return ok = false;
        if (Uncompressed(sd)) {
            Level level{RowBytes(sd), sd.dwHeight, {}};
            level.data.resize(size_t(level.rowBytes) * level.rows);
            for (uint32_t y = 0; y < level.rows; ++y)
                std::memcpy(level.data.data() + size_t(y) * level.rowBytes,
                            static_cast<const uint8_t*>(sd.lpSurface) + size_t(y) * sd.lPitch, level.rowBytes);
            out->bytes += level.data.size();
            out->levels.push_back(std::move(level));
        } else {
            ok = false;
        }
        s->Unlock(nullptr);
        return ok;
    });
    return ok && !out->levels.empty();
}

// Copies remembered levels in; false (nothing usable written) if the texture's levels don't match them.
bool Write(IDirectDrawSurface7* top, const Entry& e)
{
    size_t i = 0;
    bool ok = true;
    ForEachLevel(top, [&](IDirectDrawSurface7* s) {
        if (i >= e.levels.size()) return ok = false;
        const Level& level = e.levels[i++];
        DDSURFACEDESC2 sd{};
        sd.dwSize = sizeof(sd);
        if (FAILED(s->Lock(nullptr, &sd, DDLOCK_WAIT | DDLOCK_WRITEONLY, nullptr))) return ok = false;
        if (Uncompressed(sd) && RowBytes(sd) == level.rowBytes && sd.dwHeight == level.rows) {
            for (uint32_t y = 0; y < level.rows; ++y)
                std::memcpy(static_cast<uint8_t*>(sd.lpSurface) + size_t(y) * sd.lPitch,
                            level.data.data() + size_t(y) * level.rowBytes, level.rowBytes);
        } else {
            ok = false;
        }
        s->Unlock(nullptr);
        return ok;
    });
    return ok && i == e.levels.size();
}

void Count(bool hit)
{
    ++(hit ? g_hits : g_misses);
    const uint32_t n = g_hits + g_misses;
    if ((n & (n - 1)) == 0 && n >= 64)
        Log("texture cache: %u of %u bitmaps converted before (%.1f MB kept, %zu textures)", g_hits, n,
            g_bytes / 1048576.0, g_lru.size());
}

}  // namespace cache

void Tidy(vc10::String& s) { Internal<void(__fastcall*)(void*, void*, uint8_t, uint32_t)>(kTidy)(&s, nullptr, 1, 0); }

}  // namespace

void SetModule(HMODULE orig) { g_orig = orig; }

void __cdecl EnableCompression(bool on)
{
    const uint32_t supported = Internal<uint32_t(__cdecl*)()>(kCompressionSupported)();
    G<uint8_t>(kCompression) = uint8_t((supported != 1) && on);
}

void* __fastcall CreateTexture(Creator* self, void*, void* bitmapRaw, const char* name)
{
    lbitmap::Bitmap* bitmap = static_cast<lbitmap::Bitmap*>(bitmapRaw);
    if (G<int32_t>(kSupport) == -1) G<int32_t>(kSupport) = Internal<uint8_t(__cdecl*)()>(kFormatSupport)() != 0;
    const int32_t divisor = self->divisor;
    uint32_t width = bitmap->width, height = bitmap->height;
    if (divisor != 1) {
        height = uint32_t(int32_t(height) / divisor);
        width = uint32_t(int32_t(width) / divisor);
    }
    lbitmap::Scale(bitmap, nullptr, width, height);
    if (bitmap->state != 0) {                            // the loader's error: a message, and no surface
        vc10::String message;
        message.init();
        const int32_t state = bitmap->state;
        const char* format = state == 1   ? "No support for extension found in %s!"
                             : state == 2 ? "Bad texture file %s!"
                             : state == 3 ? "Memory error opening texture file %s!"
                                          : "Unknown error opening texture file %s!";
        Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&message, format, self->name.c_str());
        Tidy(message);
        return nullptr;
    }
    if (bitmap->bitsPerPixel != 0x18 && bitmap->bitsPerPixel != 0x20)
        reinterpret_cast<void(__fastcall*)(void*, void*)>((*reinterpret_cast<void***>(bitmap))[2])(bitmap, nullptr);
    const bool wasCompressed = G<uint8_t>(kCompression) != 0;
    vc10::String fileName;
    fileName.init();
    Internal<void(__fastcall*)(void*, void*, const char*)>(kAssign)(&fileName, nullptr, name);
    const bool isPng = Internal<uint8_t(__fastcall*)(void*, void*, const char*, const char*)>(kFind)(
                           &fileName, nullptr, ".png", nullptr) != 0;
    Tidy(fileName);
    if (isPng) EnableCompression(false);
    const int32_t quality = G<int32_t>(kQuality);
    const bool compressed = G<uint8_t>(kCompression) == 1;
    if (bitmap->bitsPerPixel == 0x20) {
        self->format = (compressed && G<uint8_t>(kDxt1)) ? 0x13 : (quality == 0 ? 10 : 2);
    } else if (bitmap->bitsPerPixel == 0x18) {
        const int32_t uncompressed = compressed ? 0 : 1;
        self->format = quality < 2 ? ((uncompressed - 1) & 0xe) + 4 : ((uncompressed - 1) & 0xf) + 3;
    }
    uint32_t levels = self->flag30 ? 0 : 0x100;
    uint32_t surfaceWidth = bitmap->width, surfaceHeight = bitmap->height;
    int32_t format = self->format;
    uint32_t tail = 0;
    if (surfaceWidth != surfaceHeight && G<uint8_t>(kSquared)) levels = 0x100;
    vc10::String createError, convertError;
    createError.init();
    convertError.init();
    Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&createError, "Could not create texture from file %s!",
                                                              self->name.c_str());
    Internal<void(__cdecl*)(void*, const char*, ...)>(kFormat)(&convertError, "Could not convert texture from file %s!",
                                                               self->name.c_str());
    if (G<int32_t>(kSupport) == 1 && format == 0x12) {
        self->flag38 = 1;
        self->format = 0x13;
        format = 0x13;
    }
    void* surface = orig::surface_t_surface_t(vc10::Allocate(0x88), nullptr, const_cast<char*>("Texture"));
    void* render = G<void*>(kRender);
    orig::render_t_D3DXCreateTexture(render, &levels, &surfaceWidth, &surfaceHeight, &format, nullptr, surface, &tail);
    if (self->format == 3 && format == 2) {
        self->flag38 = 1;
        self->format = 2;
    }
    // (A bitmap converted before: its levels from the cache, else D3DX's conversion, then kept - see cache above.)
    auto** dds = surface ? static_cast<IDirectDrawSurface7**>(orig::surface_t_GetSurfacePointer(surface)) : nullptr;
    const uint32_t area = surfaceWidth * surfaceHeight;
    IDirectDrawSurface7* top = cache::Enabled() && dds && bitmap->data && area <= 512 * 512 ? *dds : nullptr;
    const uint64_t key = top ? cache::Key(bitmap, format, levels, surfaceWidth, surfaceHeight, self->filter) : 0;
    bool done = false;
    if (top) {
        std::lock_guard<std::mutex> lock(cache::g_mutex);
        auto it = cache::g_index.find(key);
        if (it != cache::g_index.end() && cache::Write(top, *it->second)) {
            cache::g_lru.splice(cache::g_lru.begin(), cache::g_lru, it->second);
            cache::Count(true);
            done = true;
        }
    }
    if (!done) {
        orig::render_t_D3DXLoadTextureFromMemory(render, surface, 0xFFFFFFFF, bitmap->data, nullptr,
                                                 (bitmap->bitsPerPixel == 0x20) + 1, 0xFFFFFFFF, nullptr, self->filter);
        bool keep = top && area <= 128 * 128;
        if (top && !keep) {
            std::lock_guard<std::mutex> lock(cache::g_mutex);
            if (cache::g_seen.size() >= 65536) cache::g_seen.clear();
            keep = !cache::g_seen.emplace(key, 1).second;
        }
        cache::Entry entry{key, {}, 0};
        if (keep && cache::Read(top, &entry)) {
            std::lock_guard<std::mutex> lock(cache::g_mutex);
            if (!cache::g_index.count(key)) {
                cache::g_bytes += entry.bytes;
                cache::g_lru.push_front(std::move(entry));
                cache::g_index[key] = cache::g_lru.begin();
                while (cache::g_bytes > cache::kBytes && cache::g_lru.size() > 1) {
                    cache::g_bytes -= cache::g_lru.back().bytes;
                    cache::g_index.erase(cache::g_lru.back().key);
                    cache::g_lru.pop_back();
                }
            }
            cache::Count(false);
        }
    }
    EnableCompression(wasCompressed);
    Tidy(convertError);
    Tidy(createError);
    return surface;
}

void* __fastcall CreateFromStream(Creator* self, void*, void* stream, const char* name)
{
    lbitmap::Bitmap* bitmap = lbitmap::Load(static_cast<lbitmap::Stream*>(stream), name, 0);
    void* surface = CreateTexture(self, nullptr, bitmap, name);
    if (bitmap)
        reinterpret_cast<void*(__fastcall*)(void*, void*, uint8_t)>((*reinterpret_cast<void***>(bitmap))[0])(bitmap,
                                                                                                             nullptr, 1);
    return surface;
}

void* __fastcall Process(Creator* self, void*)
{
    if (self->bitmap) return CreateTexture(self, nullptr, self->bitmap, self->name.c_str());
    return CreateFromStream(self, nullptr, self->stream, self->name.c_str());
}

void* __fastcall CtorStream(Creator* self, void*, void* stream, const char* name, int32_t divisor)
{
    serialize::Get().construct(self, nullptr);
    SetVtable(self);
    self->name.init();
    self->stream = stream;
    self->bitmap = nullptr;
    self->flag38 = 0;
    self->divisor = divisor;
    self->filter = 2;
    Internal<void(__cdecl*)(void*, const char*)>(kAssignOwn)(&self->name, name ? name : "");
    self->flag30 = 1;
    if (Internal<uint32_t(__cdecl*)()>(kCompressionSupported)() == 0) G<uint8_t>(kCompression) = 0;
    return self;
}

void* __fastcall CtorBitmap(Creator* self, void*, void* bitmap, const char* name, int32_t divisor)
{
    serialize::Get().construct(self, nullptr);
    SetVtable(self);
    self->name.init();
    self->stream = nullptr;
    self->bitmap = bitmap;
    self->flag38 = 0;
    self->divisor = divisor;
    self->filter = 2;
    Internal<void(__cdecl*)(void*, const char*)>(kAssignOwn)(&self->name, name ? name : "");
    self->flag30 = 1;
    if (Internal<uint32_t(__cdecl*)()>(kCompressionSupported)() == 0) G<uint8_t>(kCompression) = 0;
    return self;
}

void* __fastcall CtorArchive(Creator* self, void*, void* archive)
{
    orig::TextureCreator_TextureCreator(self, archive);
    SetVtable(self);
    self->name.init();
    self->divisor = 1;
    self->flag38 = 0;
    self->filter = 2;
    serialize::Get().getStream(archive, nullptr);
    if (Internal<uint32_t(__cdecl*)()>(kCompressionSupported)() == 0) G<uint8_t>(kCompression) = 0;
    return self;
}

void* __cdecl Instantiate(void* archive)
{
    Creator* self = static_cast<Creator*>(vc10::Allocate(0x40));
    if (self) CtorArchive(self, nullptr, archive);
    return self;
}

void __fastcall Archive(Creator* self, void*, void* archive)
{
    orig::TextureCreator_Archive(self, archive);
    serialize::Get().getStream(archive, nullptr);
}

void __fastcall ArchiveTexture(void* self, void*, void* archive)
{
    orig::RTexture_t_Archive(self, archive);
    serialize::Get().getStream(archive, nullptr);
}

void __fastcall Dtor(Creator* self, void*)
{
    SetVtable(self);
    Tidy(self->name);
    serialize::Get().destroy(self, nullptr);
}

void* __fastcall DeletingDtor(Creator* self, void*, uint8_t flags)
{
    Dtor(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void* __fastcall DeletingDtorBase(Creator* self, void*, uint8_t flags)
{
    serialize::Get().destroy(self, nullptr);
    if (flags & 1) vc10::Free(self);
    return self;
}

void* __fastcall NameAccessor(Creator* self, void*) { return &self->name; }
uint8_t __fastcall FlagAccessor(Creator* self, void*) { return self->flag38; }

// The TextureCreator base (FUN_1004763c / FUN_1004767a), which the stream creator's own archive constructor calls:
// the Serializable_c constructor / Archive, the TextureCreator vtable, then the archive's stream.
void* __fastcall BaseConstruct(Creator* self, void*, void* archive)
{
    serialize::Get().constructFrom(self, nullptr, archive);
    self->vtable = reinterpret_cast<uint8_t*>(g_orig) + 0x8AA2C;   // TextureCreator::vftable
    serialize::Get().getStream(archive, nullptr);
    return self;
}
void __fastcall BaseArchive(Creator* self, void*, void* archive)
{
    using ArchiveFn = void(__fastcall*)(void*, void*, void*);
    auto archive_ = *reinterpret_cast<ArchiveFn*>(reinterpret_cast<uint8_t*>(g_orig) + 0x8A36C);   // fun::Serializable_c::Archive
    archive_(self, nullptr, archive);
    serialize::Get().getStream(archive, nullptr);
}

// vsnprintf into a std::string (0x1988f): a 256- then 512-byte stack buffer, and if both truncate, the string grown
// in 0x800 steps (FUN_1001798f / FUN_10017969) up to four times; the result assigned with the string's own assign.
void __fastcall FormatInto(vc10::String* self, void*, const char* format, va_list ap)
{
    char stack256[256], stack512[512];
    const int inSmall = _vsnprintf(stack256, 0xff, format, ap);
    if (inSmall >= 0) {
        stack256[inSmall] = 0;
        Internal<void(__cdecl*)(void*, const char*)>(kAssignOwn)(self, stack256);
        return;
    }
    const int inMedium = _vsnprintf(stack512, 0x1ff, format, ap);
    if (inMedium >= 0) {
        stack512[inMedium] = 0;
        Internal<void(__cdecl*)(void*, const char*)>(kAssignOwn)(self, stack512);
        return;
    }
    int remaining = 4;
    uint32_t size = 0xa00;
    do {
        char* buffer = static_cast<char*>(
            Internal<void*(__fastcall*)(void*, void*, uint32_t)>(kStlGrow)(self, nullptr, size + 1));
        const int written = _vsnprintf(buffer, size, format, ap);
        if (written >= 0) {
            Internal<void(__fastcall*)(void*, void*, uint32_t, uint8_t)>(kStlResize)(self, nullptr, uint32_t(written),
                                                                                    0);
            return;
        }
        --remaining;
        size += 0x800;
    } while (remaining > 0);
}

void __cdecl Format(void* out, const char* format, ...)
{
    va_list ap;
    va_start(ap, format);
    FormatInto(static_cast<vc10::String*>(out), nullptr, format, ap);
    va_end(ap);
}

void Install(HMODULE orig)
{
    if (GetMode("Device", Mode::On) != Mode::On) return;
    g_orig = orig;
    struct Entry {
        uint32_t rva;
        void* target;
        const char* what;
    };
#define FN(f) reinterpret_cast<void*>(&f)
    const Entry entries[] = {
        {0x17EB0, FN(ArchiveTexture), "RTexture_t archive wrapper (FUN_10017eb0)"},
        {0x191A0, FN(Archive), "TextureStreamCreator::Archive"},
        {0x191B8, FN(EnableCompression), "TextureStreamCreator::EnableCompression"},
        {0x1920F, FN(CtorArchive), "TextureStreamCreator::TextureStreamCreator(archive)"},
        {0x19289, FN(Instantiate), "TextureStreamCreator::Instantiate"},
        {0x192BE, FN(CtorStream), "TextureStreamCreator::TextureStreamCreator(stream, name, int)"},
        {0x19351, FN(CtorBitmap), "TextureStreamCreator::TextureStreamCreator(bitmap, name, int)"},
        {0x193E4, FN(CreateTexture), "TextureStreamCreator::CreateTexture(LBitmap_t*, name)"},
        {0x1973A, FN(CreateFromStream), "TextureStreamCreator::CreateTexture(stream, name)"},
        {0x19774, FN(DeletingDtorBase), "TextureStreamCreator deleting destructor, base (FUN_10019774)"},
        {0x1984A, FN(NameAccessor), "TextureStreamCreator name (FUN_1001984a)"},
        {0x1984E, FN(FlagAccessor), "TextureStreamCreator flag (FUN_1001984e)"},
        {0x19852, FN(Dtor), "TextureStreamCreator destructor (FUN_10019852)"},
        {0x4763C, FN(BaseConstruct), "TextureCreator::TextureCreator(archive)"},
        {0x4767A, FN(BaseArchive), "TextureCreator::Archive"},
        {0x19870, FN(DeletingDtor), "TextureStreamCreator deleting destructor (FUN_10019870)"},
        {0x1988F, FN(FormatInto), "format into a std::string (FUN_1001988f)"},
        {0x1994C, FN(Format), "format with varargs (FUN_1001994c)"},
        {0x19960, FN(Process), "TextureStreamCreator vtable Process (FUN_10019960)"},
    };
#undef FN
    int installed = 0;
    for (const Entry& e : entries) installed += Replace(orig, e.rva, e.target, e.what) ? 1 : 0;
    Log("texture_stream: %d of %d functions native", installed, int(sizeof(entries) / sizeof(entries[0])));
}

}  // namespace rnative::texturestream
