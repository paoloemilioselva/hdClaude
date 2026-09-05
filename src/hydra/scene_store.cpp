#include "scene_store.h"

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
        const auto prototype = static_cast<std::uint32_t>(scene.prototypes.size());
        scene.prototypes.push_back(mesh.prototype);

        auto found = materialIndex.find(mesh.material);
        const std::uint32_t material =
            found == materialIndex.end() ? 0u : found->second;

        for (const hdclaude::Transform3x4& transform : mesh.transforms) {
            hdclaude::MeshInstance instance;
            instance.prototype = prototype;
            instance.transform = transform;
            instance.material = material;
            instance.visible = true;
            scene.instances.push_back(instance);
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
    return reports;
}

PXR_NAMESPACE_CLOSE_SCOPE
