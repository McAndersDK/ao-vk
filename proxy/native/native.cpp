// The replacements, installed once randy31_orig.dll is loaded.
#include "native/native.h"
#include "native/cat_anim.h"
#include "native/cat_mesh.h"
#include "native/cat_query.h"
#include "native/cat_render.h"
#include "native/cat_skin.h"
#include "native/device.h"
#include "native/orig_api.gen.h"
#include "native/scene.h"
#include "native/viewport.h"
#include "native/refframe.h"
#include "native/visual.h"
#include "native/camera.h"
#include "native/mesh.h"
#include "native/mesh_data.h"
#include "native/occluder.h"
#include "native/cat_life.h"
#include "native/cat_pick.h"
#include "native/cat_data.h"
#include "native/cat_anim_data.h"
#include "native/bvolume.h"
#include "native/gone_trap.h"
#include "native/keyframe.h"
#include "native/sprite.h"
#include "native/shadowlands.h"
#include "native/color.h"
#include "native/pixfmt.h"
#include "native/lbitmap.h"
#include "native/helpers.h"
#include "native/texture_stream.h"
#include "native/faf.h"
#include "native/rshadow.h"
#include "native/connector.h"
#include "native/cat_status.h"
#include "native/rgrid.h"
#include "native/timer.h"
#include "native/funcom.h"
#include "native/findobject.h"
#include "native/popin.h"
#include "native/d3dx.h"

namespace rnative {

namespace {

// Before 0.5 every part defaulted to off, and the first run wrote `off` into randy-vk.ini - so a new default never
// reached an existing install. Once ([Native] Version < 1): a part that reads off goes on. (Version=1 marks it done;
// a part turned off after that stays off.)
void MigrateDefaults()
{
    const char* ini = IniPath();
    if (!ini[0] || GetPrivateProfileIntA("Native", "Version", 0, ini) >= 1)
        return;
    static const char* const kParts[] = {"Skin", "Anim", "Visuals", "CatRender", "CatMesh", "CatQuery", "Device", "Scene"};
    int switched = 0;
    for (const char* part : kParts) {
        char value[16] = "";
        GetPrivateProfileStringA("Native", part, "", value, sizeof(value), ini);
        if (value[0] && !_stricmp(value, "off")) {
            WritePrivateProfileStringA("Native", part, "on", ini);
            ++switched;
        }
    }
    WritePrivateProfileStringA("Native", "Version", "1", ini);
    if (switched)
        Log("settings: %d [Native] parts switched on (the new default; turn one off again in randy-vk.ini if it misbehaves)",
            switched);
}

}  // namespace

void Install(HMODULE orig)
{
    static bool done = false;
    if (done || !orig)
        return;
    done = true;
    SetOriginal(orig);
    InstallCrashLog();
    MigrateDefaults();
    skin::Install(orig);
    anim::Install(orig);
    scene::Install(orig);
    popin::Install(orig);
    catrender::Install(orig);
    catmesh::Install(orig);
    catquery::Install(orig);
    device::Install(orig);
    viewport::Install(orig);
    refframe::Install(orig);
    visual::Install(orig);
    camera::Install(orig);
    mesh::Install(orig);
    meshdata::Install(orig);
    occluder::Install(orig);
    catlife::Install(orig);
    catpick::Install(orig);
    catdata::Install(orig);
    cat_anim_data::Install(orig);
    bvolume::Install(orig);
    keyframe::Install(orig);
    sprite::Install(orig);
    shadowlands::Install(orig);
    color::Install(orig);
    pixfmt::Install(orig);
    lbitmap::Install(orig);
    helpers::Install(orig);
    texturestream::Install(orig);
    faf::Install(orig);
    rshadow::Install(orig);
    connector::Install(orig);
    catstatus::Install(orig);
    rgrid::Install(orig);
    timer::Install(orig);
    funcom::Install(orig);
    findobject::Install(orig);
    d3dx::Install(orig);
    // After the installs, so each rva resolves to our function (else the original's): orig:: is then independent of
    // the original's export table.
    if (!orig::Init())
        Log("some of randy31_orig's exports are missing - unknown client build");
    gonetrap::Install(orig);                        // last: over what the replacements left
}

}  // namespace rnative
