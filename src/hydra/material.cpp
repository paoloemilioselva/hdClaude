#include "material.h"

#include "material_compiler.h"
#include "render_param.h"

#include <chrono>
#include "scene_store.h"
#include "texture_loader.h"

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
        const auto compileStart = std::chrono::steady_clock::now();
        HdClaudeMaterialCompiler::Result compiled =
            param->MaterialCompiler()->Compile(
                resource.UncheckedGet<HdMaterialNetworkMap>(), id,
                GfVec3f(0.5f, 0.5f, 0.5f));
        // Generation and SPIR-V compilation are one cost from out here, and
        // there is no moment between them worth reporting separately.
        const double compileMs = std::chrono::duration<double, std::milli>(
                                     std::chrono::steady_clock::now() -
                                     compileStart)
                                     .count();
        if (HdClaudeStageStats* stats = param->StageStats()) {
            HdClaudeAddMilliseconds(stats->materialMilliseconds, compileMs);
            stats->materialsCompiled.fetch_add(1, std::memory_order_relaxed);
        }
        entry.compiled = std::move(compiled.material);
        entry.fallbackReason = std::move(compiled.fallbackReason);

        // The generator numbered this material's samplers from zero; the pool
        // turns each asset path into a slot shared with every other material
        // that names the same image.
        if (HdClaudeTexturePool* pool = param->TexturePool()) {
            entry.compiled.textureSlots.reserve(compiled.texturePaths.size());
            for (const HdClaudeMaterialCompiler::TextureRequest& texture :
                 compiled.texturePaths) {
                // Timed around Acquire rather than around the decoder, because
                // the pool shares an image between every material that names
                // it: the second ask costs a lookup, and counting it as a load
                // would say a scene decoded far more than it did.
                const std::size_t before = pool->Images().size();
                const auto textureStart = std::chrono::steady_clock::now();
                const std::uint32_t slot =
                    pool->Acquire(texture.path, texture.colorSpace);
                const double textureMs =
                    std::chrono::duration<double, std::milli>(
                        std::chrono::steady_clock::now() - textureStart)
                        .count();
                entry.compiled.textureSlots.push_back(slot);

                if (HdClaudeStageStats* stats = param->StageStats()) {
                    HdClaudeAddMilliseconds(stats->textureMilliseconds, textureMs);
                    if (pool->Images().size() > before &&
                        slot < pool->Images().size()) {
                        const hdclaude::TextureImage& image =
                            pool->Images()[slot];
                        stats->texturesLoaded.fetch_add(1,
                                                        std::memory_order_relaxed);
                        stats->textureBytes.fetch_add(image.texels.size(),
                                                      std::memory_order_relaxed);
                    }
                }
            }
        }
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
