// Static meshes natively (mesh.cpp): randy-vk.ini [Native] Scene=on.
#pragma once

#include "native/native.h"

namespace rnative::mesh {

void Install(HMODULE orig);
// Shadow-only drawing (randy-vk RVK_ShadowAll): a static mesh the game culled - out of view, hidden by a hill - but
// within `range` of the camera is drawn anyway with ShadowOnly() set: the renderer records it as a sun shadow caster
// and draws nothing (its shadows then hold all that stands near, whichever way the camera looks). 0: off.
void SetShadowOnlyRange(float range, float minRadius);
bool ShadowOnly();

}  // namespace rnative::mesh
