#include "material.h"

#include "material_compiler.h"
#include "render_param.h"
#include "scene_store.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/imaging/hd/sceneDelegate.h"

PXR_NAMESPACE_OPEN_SCOPE

HdClaudeMaterial::HdClaudeMaterial(const SdfPath& id) : HdMaterial(id) {}
HdClaudeMaterial::~HdClaudeMaterial() = default;

HdDirtyBits HdClaudeMaterial::GetInitialDirtyBitsMask() const
{
    return HdMaterial::AllDirty;
}

void HdClaudeMaterial::Finalize(HdRenderParam* renderParam)
{
    if (auto* param = static_cast<HdClaudeRenderParam*>(renderParam)) {
        param->SceneStore()->RemoveMaterial(GetId());
    }
}

void HdClaudeMaterial::Sync(HdSceneDelegate* sceneDelegate,
                            HdRenderParam* renderParam, HdDirtyBits* dirtyBits)
{
    auto* param = static_cast<HdClaudeRenderParam*>(renderParam);
    if (param == nullptr || sceneDelegate == nullptr) {
        return;
    }
    if ((*dirtyBits & HdMaterial::DirtyResource) == 0 &&
        (*dirtyBits & HdMaterial::DirtyParams) == 0) {
        *dirtyBits = HdMaterial::Clean;
        return;
    }

    const SdfPath& id = GetId();
    const VtValue resource = sceneDelegate->GetMaterialResource(id);

    HdClaudeMaterialEntry entry;
    if (resource.IsHolding<HdMaterialNetworkMap>()) {
        // The grey here is only reached when the authored network cannot be
        // used; a mesh's own displayColor fallback is handled by the mesh,
        // which is the prim that knows it.
        HdClaudeMaterialCompiler::Result compiled =
            param->MaterialCompiler()->Compile(
                resource.UncheckedGet<HdMaterialNetworkMap>(), id,
                GfVec3f(0.5f, 0.5f, 0.5f));
        entry.compiled = std::move(compiled.material);
        entry.fallbackReason = std::move(compiled.fallbackReason);
    } else {
        entry.fallbackReason =
            "the material prim carries no HdMaterialNetworkMap";
    }

    if (!entry.fallbackReason.empty()) {
        // Said once, at Sync, not once per frame: a per-frame warning buries
        // the first occurrence and makes the log useless.
        TF_WARN("hdClaude: <%s> is not shaded as authored: %s", id.GetText(),
                entry.fallbackReason.c_str());
    }

    param->SceneStore()->PublishMaterial(id, std::move(entry));
    *dirtyBits = HdMaterial::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
