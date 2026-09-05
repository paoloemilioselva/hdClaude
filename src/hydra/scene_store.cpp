#include "scene_store.h"

#include <algorithm>

PXR_NAMESPACE_OPEN_SCOPE

void HdClaudeSceneStore::PublishMesh(const SdfPath& id, HdClaudeMeshEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _meshes[id] = std::move(entry);
    ++_revision;
}

void HdClaudeSceneStore::RemoveMesh(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_meshes.erase(id) > 0) {
        ++_revision;
    }
}

void HdClaudeSceneStore::PublishMaterial(const SdfPath& id, HdClaudeMaterialEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _materials[id] = std::move(entry);
    ++_revision;
}

void HdClaudeSceneStore::RemoveMaterial(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_materials.erase(id) > 0) {
        ++_revision;
    }
}

void HdClaudeSceneStore::PublishLight(const SdfPath& id, HdClaudeLightEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _lights[id] = std::move(entry);
    ++_revision;
}

void HdClaudeSceneStore::RemoveLight(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_lights.erase(id) > 0) {
        ++_revision;
    }
}

bool HdClaudeSceneStore::HasMaterial(const SdfPath& id) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _materials.count(id) > 0;
}

std::uint64_t HdClaudeSceneStore::Revision() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _revision;
}

void HdClaudeSceneStore::SetFallbackMaterial(hdclaude::CompiledMaterial material)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _fallback = std::move(material);
    _hasFallback = !_fallback.spirv.empty();
    ++_revision;
}

bool HdClaudeSceneStore::HasFallbackMaterial() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _hasFallback;
}

hdclaude::Scene HdClaudeSceneStore::Snapshot(
    std::vector<hdclaude::CompiledMaterial>& materials) const
{
    std::lock_guard<std::mutex> lock(_mutex);

    hdclaude::Scene scene;
    materials.clear();

    // Index 0 is the fallback. A mesh whose material failed to compile, or that
    // has none, still renders -- a dropped prim is much harder to diagnose than
    // an obviously untextured one.
    materials.push_back(_fallback);

    std::map<SdfPath, std::uint32_t> materialIndex;
    for (const auto& [path, entry] : _materials) {
        if (entry.compiled.spirv.empty()) {
            continue;
        }
        materialIndex[path] = static_cast<std::uint32_t>(materials.size());
        materials.push_back(entry.compiled);
    }

    scene.prototypes.reserve(_meshes.size());
    for (const auto& [path, mesh] : _meshes) {
        if (mesh.prototype.indices.empty() || !mesh.visible) {
            continue;
        }
        auto resolve = [&](const SdfPath& binding) -> std::uint32_t {
            const auto found = materialIndex.find(binding);
            return found == materialIndex.end() ? 0u : found->second;
        };

        const auto prototype = static_cast<std::uint32_t>(scene.prototypes.size());
        scene.prototypes.push_back(mesh.prototype);

        const std::uint32_t material = resolve(mesh.material);

        // GeomSubsets become a per-triangle material index. Resolved here
        // rather than at Sync because only the store knows what index a
        // material path ended up with, and a subset may name a material whose
        // prim has not been synced yet -- that triangle falls back to the
        // mesh's own binding rather than disappearing.
        if (!mesh.triangleSubsets.empty()) {
            hdclaude::MeshPrototype& published = scene.prototypes.back();
            published.triangleMaterials.resize(mesh.triangleSubsets.size(),
                                               material);
            for (std::size_t i = 0; i < mesh.triangleSubsets.size(); ++i) {
                const int subset = mesh.triangleSubsets[i];
                published.triangleMaterials[i] =
                    (subset >= 0 && static_cast<std::size_t>(subset) <
                                        mesh.subsetMaterials.size())
                        ? resolve(mesh.subsetMaterials[subset])
                        : material;
            }
        }

        for (const hdclaude::Transform3x4& transform : mesh.transforms) {
            hdclaude::MeshInstance instance;
            instance.prototype = prototype;
            instance.transform = transform;
            instance.material = material;
            instance.visible = true;
            scene.instances.push_back(instance);
        }
    }

    // Lights, and the environment a dome light supplies.
    //
    // Several dome lights are legal in USD and rare in practice; their
    // radiances are summed, which is what a renderer that treated each as a
    // real emitter would arrive at, rather than silently honouring one.
    for (const auto& [path, entry] : _lights) {
        if (entry.isDome) {
            if (!scene.hasDomeLight) {
                scene.hasDomeLight = true;
                scene.environmentColor[0] = 0.0f;
                scene.environmentColor[1] = 0.0f;
                scene.environmentColor[2] = 0.0f;
            }
            for (int i = 0; i < 3; ++i) {
                scene.environmentColor[i] += entry.environmentColor[i];
            }
            // The first dome with a map supplies it. Two textured domes is not
            // a thing a renderer can composite meaningfully, so the second is
            // reported rather than silently blended.
            if (entry.domeTexture >= 0 && scene.domeTexture < 0) {
                scene.domeTexture = entry.domeTexture;
                std::copy(std::begin(entry.domeWorldToLight),
                          std::end(entry.domeWorldToLight),
                          std::begin(scene.domeWorldToLight));
            }
        } else {
            scene.lights.push_back(entry.light);
        }
    }

    scene.revision = _revision;
    return scene;
}

std::vector<std::string> HdClaudeSceneStore::FallbackReports() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<std::string> reports;
    for (const auto& [path, entry] : _materials) {
        if (!entry.fallbackReason.empty()) {
            reports.push_back(path.GetString() + ": " + entry.fallbackReason);
        }
    }
    for (const auto& [path, entry] : _lights) {
        if (!entry.report.empty()) {
            reports.push_back(path.GetString() + ": " + entry.report);
        }
    }
    return reports;
}

PXR_NAMESPACE_CLOSE_SCOPE
