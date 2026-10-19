#include "material.h"

#include "material_compiler.h"
#include "render_param.h"

#include <chrono>
#include "scene_store.h"
#include "texture_loader.h"
#include "trace.h"

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

    // Published pending, not compiled. Hydra syncs every material in the
    // stage, bound or not, and syncs them before any rprim, so nothing here can
    // say whether this one will be used; the render pass compiles it once a
    // visible prim binds it (HdClaudeCompileBoundMaterials). An edit to a
    // bound material comes back through here and is compiled again on the next
    // frame, which is when it was always going to be seen.
    HdClaudeMaterialEntry entry;
    if (resource.IsHolding<HdMaterialNetworkMap>()) {
        entry.network = resource.UncheckedGet<HdMaterialNetworkMap>();
        entry.pending = true;
    } else {
        // Nothing to defer: there is no network to compile, and saying so at
        // Sync is as useful as saying it later.
        entry.fallbackReason =
            "the material prim carries no HdMaterialNetworkMap";
        TF_WARN("hdClaude: <%s> is not shaded as authored: %s", id.GetText(),
                entry.fallbackReason.c_str());
    }

    param->SceneStore()->PublishMaterial(id, std::move(entry));
    *dirtyBits = HdMaterial::Clean;
}

namespace {

/// A compiled material whose textures are still to be acquired.
struct CompiledPending {
    HdClaudePendingMaterial pending;
    HdClaudeMaterialEntry entry;
    std::vector<HdClaudeMaterialCompiler::TextureRequest> surfaceTextures;
    std::vector<HdClaudeMaterialCompiler::TextureRequest> displaceTextures;
};

CompiledPending CompileMaterial(HdClaudeRenderParam* param,
                                const HdClaudePendingMaterial& pending)
{
    CompiledPending result;
    result.pending = pending;
    const SdfPath& id = pending.id;

    // The grey here is only reached when the authored network cannot be used;
    // a mesh's own displayColor fallback is handled by the mesh, which is the
    // prim that knows it.
    const auto compileStart = std::chrono::steady_clock::now();
    HdClaudeMaterialCompiler::Result compiled =
        param->MaterialCompiler()->Compile(pending.network, id,
                                           GfVec3f(0.5f, 0.5f, 0.5f));
    // Generation and SPIR-V compilation are one cost from out here, and there
    // is no moment between them worth reporting separately.
    const double compileMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() -
                                 compileStart)
                                 .count();
    if (HdClaudeStageStats* stats = param->StageStats()) {
        HdClaudeAddMilliseconds(stats->materialMilliseconds, compileMs);
        (compiled.reused ? stats->materialsReused : stats->materialsCompiled)
            .fetch_add(1, std::memory_order_relaxed);
    }
    if (compiled.reused) {
        HdClaudeTrace("material <%s>: same network as one already compiled",
                      id.GetText());
    }
    result.entry.compiled = std::move(compiled.material);
    result.entry.fallbackReason = std::move(compiled.fallbackReason);
    result.entry.shareKey = std::move(compiled.contentKey);
    result.surfaceTextures = std::move(compiled.texturePaths);
    result.displaceTextures = std::move(compiled.displacementTexturePaths);

    if (!result.entry.fallbackReason.empty()) {
        // Said once, when compiled, not once per frame: a per-frame warning
        // buries the first occurrence and makes the log useless.
        TF_WARN("hdClaude: <%s> is not shaded as authored: %s", id.GetText(),
                result.entry.fallbackReason.c_str());
    }
    return result;
}

/// Acquire every compiled material's textures in one parallel batch, and give
/// each material its pool slots.
///
/// One batch rather than one per material, so the decode runs across every
/// image the frame needs at once: a material naming eight large maps would
/// otherwise keep eight cores busy at most, and a scene of single-map
/// materials only one.
///
/// Each material's generator numbered its samplers from zero, twice over: the
/// surface and the displacement are generated separately, so a height map is
/// index 0 of the displacement whatever the surface reads. The pool turns
/// every asset path into a slot shared with every other material that names
/// the same image, so an image both programs name is decoded once.
void AcquireTextures(HdClaudeRenderParam* param,
                     std::vector<CompiledPending>& compiled)
{
    HdClaudeTexturePool* pool = param->TexturePool();
    if (pool == nullptr) {
        return;
    }

    std::vector<HdClaudeTexturePool::Request> requests;
    for (const CompiledPending& material : compiled) {
        for (const auto* textures :
             {&material.surfaceTextures, &material.displaceTextures}) {
            for (const HdClaudeMaterialCompiler::TextureRequest& texture :
                 *textures) {
                requests.push_back({texture.path, texture.colorSpace});
            }
        }
    }
    if (requests.empty()) {
        return;
    }

    // Timed around the batch: with the decode spread over every core, the
    // time a frame waits is the wall clock, not the sum of the decodes.
    HdClaudeTexturePool::AcquireReport report;
    const auto textureStart = std::chrono::steady_clock::now();
    const std::vector<std::uint32_t> slots = pool->AcquireAll(requests, &report);
    const double textureMs = std::chrono::duration<double, std::milli>(
                                 std::chrono::steady_clock::now() - textureStart)
                                 .count();
    if (HdClaudeStageStats* stats = param->StageStats()) {
        HdClaudeAddMilliseconds(stats->textureMilliseconds, textureMs);
        stats->texturesLoaded.fetch_add(report.decoded,
                                        std::memory_order_relaxed);
        stats->textureBytes.fetch_add(report.bytes, std::memory_order_relaxed);
    }
    HdClaudeTrace("textures: %zu requested, %zu decoded in %.0f ms",
                  requests.size(), report.decoded, textureMs);

    std::size_t next = 0;
    for (CompiledPending& material : compiled) {
        hdclaude::CompiledMaterial& program = material.entry.compiled;
        for (std::size_t i = 0; i < material.surfaceTextures.size(); ++i) {
            program.textureSlots.push_back(slots[next++]);
        }
        for (std::size_t i = 0; i < material.displaceTextures.size(); ++i) {
            program.displaceTextureSlots.push_back(slots[next++]);
        }
    }
}

}  // namespace

void HdClaudeCompileBoundMaterials(HdClaudeRenderParam* param)
{
    if (param == nullptr || param->MaterialCompiler() == nullptr) {
        return;
    }
    HdClaudeSceneStore* store = param->SceneStore();

    std::vector<CompiledPending> compiled;
    for (const HdClaudePendingMaterial& pending :
         store->BoundPendingMaterials()) {
        HdClaudeTrace("material <%s>: bound, compiling", pending.id.GetText());
        compiled.push_back(CompileMaterial(param, pending));
    }
    AcquireTextures(param, compiled);
    for (CompiledPending& material : compiled) {
        store->CompletePendingMaterial(material.pending.id,
                                       material.pending.generation,
                                       std::move(material.entry));
    }
}

PXR_NAMESPACE_CLOSE_SCOPE
