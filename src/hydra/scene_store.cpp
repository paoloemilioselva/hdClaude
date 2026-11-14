#include "scene_store.h"

#include "trace.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/imaging/hd/collectionExpressionEvaluator.h"
#include "pxr/imaging/hd/tokens.h"

#include <algorithm>
#include <cmath>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// Luminance of a linear Rec. 709 radiance, which is what UsdLux's
/// `luminance(color) * intensity * 2^exposure` is: the light's illuminant is
/// scaled to D65's luminous power (light.cpp), so its colour carries its
/// luminance.
double RadianceLuminance(const float* rgb)
{
    return 0.2126729 * rgb[0] + 0.7151522 * rgb[1] + 0.0721750 * rgb[2];
}

/// What a unit of luminance from this light delivers toward its target:
/// lumens for a power, lux for an illuminance.
double Delivery(const HdClaudeLightEntry& entry, double metersPerUnit)
{
    const HdClaudeLightBrightness& brightness = entry.brightness;
    if (brightness.cachedMetersPerUnit == metersPerUnit) {
        return brightness.cachedDelivery;
    }
    double delivery = 0.0;
    if (entry.light.type == static_cast<std::uint32_t>(hdclaude::LightType::Distant)) {
        // The cone the path tracer samples, whose radius is the authored one
        // with a floor (light.cpp), so the floor is honoured too.
        delivery = hdclaude::DistantIlluminancePerLuminance(entry.light.angularRadius);
    } else if (brightness.target == HdClaudeLightBrightness::Target::Power) {
        delivery = hdclaude::EmittedPowerPerLuminance(brightness.emitter) *
                   metersPerUnit * metersPerUnit;
    } else {
        delivery = hdclaude::OnAxisIlluminancePerLuminance(
            brightness.emitter, brightness.distanceMeters / metersPerUnit);
    }
    brightness.cachedMetersPerUnit = metersPerUnit;
    brightness.cachedDelivery = delivery;
    return delivery;
}

/// The factor that makes `delivered` (per unit of the light's luminance as
/// authored) equal the target, or a negative number when nothing can.
double BrightnessScale(double target, double luminance, double delivery)
{
    const double delivered = luminance * delivery;
    if (!(delivered > 0.0) || !std::isfinite(delivered)) {
        return -1.0;
    }
    return target / delivered;
}

std::string Unmet(const SdfPath& path, const std::string& statement)
{
    return path.GetString() + ": " + statement +
           " cannot be met, because the light as authored delivers nothing to "
           "scale (its colour, intensity or shaping is zero, or the surface it "
           "is measured on is inside it); it emits as authored";
}

}  // namespace

void HdClaudeSceneStore::PublishMesh(const SdfPath& id, HdClaudeMeshEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _meshes[id] = std::move(entry);
    ++_revision;
}

bool HdClaudeSceneStore::UpdateMeshPlacement(
    const SdfPath& id, std::vector<hdclaude::Transform3x4> transforms,
    bool visible, SdfPath material,
    std::vector<std::vector<TfToken>> instanceCategories, int primId)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _meshes.find(id);
    if (found == _meshes.end()) {
        return false;
    }
    HdClaudeMeshEntry& entry = found->second;
    entry.transforms = std::move(transforms);
    entry.visible = visible;
    entry.material = std::move(material);
    entry.instanceCategories = std::move(instanceCategories);
    entry.primId = primId;
    ++_revision;
    return true;
}

void HdClaudeSceneStore::RemoveMesh(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_meshes.erase(id) > 0) {
        ++_revision;
    }
}

void HdClaudeSceneStore::PublishSplats(const SdfPath& id,
                                      HdClaudeSplatEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _splats[id] = std::move(entry);
    ++_revision;
}

void HdClaudeSceneStore::RemoveSplats(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_splats.erase(id) > 0) {
        ++_revision;
    }
}

void HdClaudeSceneStore::PublishMaterial(const SdfPath& id, HdClaudeMaterialEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    entry.generation = ++_materialGeneration;
    _materials[id] = std::move(entry);
    ++_revision;
}

std::vector<HdClaudePendingMaterial>
HdClaudeSceneStore::BoundPendingMaterials() const
{
    std::lock_guard<std::mutex> lock(_mutex);

    // The same meshes Snapshot shades: an invisible or empty mesh binds
    // nothing yet, and its material is compiled when it is shown.
    std::set<SdfPath> bound;
    for (const auto& [path, mesh] : _meshes) {
        if ((mesh.prototype.indices.empty() && mesh.prototype.segments.empty()) ||
            !mesh.visible) {
            continue;
        }
        bound.insert(mesh.material);
        bound.insert(mesh.subsetMaterials.begin(), mesh.subsetMaterials.end());
    }

    std::vector<HdClaudePendingMaterial> pending;
    for (const SdfPath& id : bound) {
        const auto found = _materials.find(id);
        if (found != _materials.end() && found->second.pending) {
            pending.push_back(
                {id, found->second.network, found->second.generation});
        }
    }
    return pending;
}

void HdClaudeSceneStore::CompletePendingMaterial(const SdfPath& id,
                                                 std::uint64_t generation,
                                                 HdClaudeMaterialEntry compiled)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto found = _materials.find(id);
    if (found == _materials.end() || found->second.generation != generation) {
        return;
    }
    compiled.generation = generation;
    found->second = std::move(compiled);
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

void HdClaudeSceneStore::PublishCullVolume(const SdfPath& id,
                                           HdClaudeCullVolumeEntry entry)
{
    std::lock_guard<std::mutex> lock(_mutex);
    const auto existing = _cullVolumes.find(id);
    // What decides instance masks: whether the volume takes part at all, which
    // rays it affects, and what it links. Invert and placement do not, and a
    // volume that only moved must not cost a scene publication.
    constexpr std::uint32_t kKinds =
        hdclaude::kCullAffectsPrimary | hdclaude::kCullAffectsSecondary;
    const bool structural =
        existing == _cullVolumes.end() ||
        existing->second.visible != entry.visible ||
        existing->second.purpose != entry.purpose ||
        (existing->second.volume.flags & kKinds) != (entry.volume.flags & kKinds) ||
        existing->second.linksEverything != entry.linksEverything ||
        existing->second.cullLink.GetText() != entry.cullLink.GetText();
    _cullVolumes[id] = std::move(entry);
    if (structural) {
        ++_revision;
    }
    ++_cullRevision;
}

void HdClaudeSceneStore::RemoveCullVolume(const SdfPath& id)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (_cullVolumes.erase(id) != 0) {
        ++_revision;
        ++_cullRevision;
    }
}

void HdClaudeSceneStore::SetSceneIndex(const HdSceneIndexBaseRefPtr& sceneIndex)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _sceneIndex = sceneIndex;
}

void HdClaudeSceneStore::SetCullVolumesEnabled(bool enabled)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (enabled != _cullVolumesEnabled) {
        _cullVolumesEnabled = enabled;
        ++_revision;
        ++_cullRevision;
    }
}

void HdClaudeSceneStore::SetHostInstanceData(bool host)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (host != _hostInstanceData) {
        _hostInstanceData = host;
        ++_revision;
    }
}

void HdClaudeSceneStore::SetStageUnits(double metersPerUnit, bool metersExplicit,
                                       bool zUp, bool upExplicit)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (metersPerUnit != _metersPerUnit || metersExplicit != _metersExplicit ||
        zUp != _zUp || upExplicit != _upExplicit) {
        _metersPerUnit = metersPerUnit;
        _metersExplicit = metersExplicit;
        _zUp = zUp;
        _upExplicit = upExplicit;
        ++_revision;
    }
}

void HdClaudeSceneStore::AddSnapshotReport(std::string report)
{
    std::lock_guard<std::mutex> lock(_mutex);
    _snapshotReports.push_back(std::move(report));
}

void HdClaudeSceneStore::SetTlasChunkSize(std::uint32_t size)
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (size != _tlasChunkSize) {
        _tlasChunkSize = size;
        ++_revision;
    }
}

std::uint64_t HdClaudeSceneStore::CullRevision() const
{
    std::lock_guard<std::mutex> lock(_mutex);
    return _cullRevision;
}

bool HdClaudeDrawSelection::IncludesPurpose(const TfToken& purpose) const
{
    if (includesPurpose) {
        return includesPurpose(purpose);
    }
    return purpose != HdRenderTagTokens->guide &&
           purpose != HdRenderTagTokens->proxy;
}

std::vector<SdfPath> HdClaudeSceneStore::_ActiveCullVolumes(
    const HdClaudeDrawSelection& selection) const
{
    std::vector<SdfPath> active;
    if (!_cullVolumesEnabled) {
        return active;
    }
    for (const auto& [path, volume] : _cullVolumes) {
        if (volume.visible && selection.IncludesPurpose(volume.purpose) &&
            active.size() < hdclaude::kMaxCullVolumes) {
            active.push_back(path);
        }
    }
    return active;
}

std::vector<hdclaude::CullVolume> HdClaudeSceneStore::CullVolumes(
    const HdClaudeDrawSelection& selection) const
{
    std::lock_guard<std::mutex> lock(_mutex);
    std::vector<hdclaude::CullVolume> volumes;
    for (const SdfPath& path : _ActiveCullVolumes(selection)) {
        volumes.push_back(_cullVolumes.at(path).volume);
    }
    return volumes;
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
    std::vector<hdclaude::CompiledMaterial>& materials,
    const HdClaudeDrawSelection& selection)
{
    std::lock_guard<std::mutex> lock(_mutex);

    hdclaude::Scene scene;
    materials.clear();

    // Index 0 is the fallback, a plain grey, reached only when the
    // displayColor material itself is missing: everything else that binds no
    // usable material is shaded with its displayColor (below). A prim still
    // renders either way -- a dropped prim is much harder to diagnose than an
    // obviously untextured one.
    materials.push_back(_fallback);

    // Cull volumes: which affect each kind of ray, and which geometry each
    // links, as one bit per volume. The bits are numbered as CullVolumes()
    // numbers the volumes, so the kernels' per-placement links and the volume
    // table agree.
    const std::vector<SdfPath> activeCullVolumes =
        _ActiveCullVolumes(selection);
    std::vector<HdCollectionExpressionEvaluator> cullLinks;
    std::uint32_t cullLinksEverything = 0;
    for (std::size_t bit = 0; bit < activeCullVolumes.size(); ++bit) {
        const HdClaudeCullVolumeEntry& volume =
            _cullVolumes.at(activeCullVolumes[bit]);
        if (volume.volume.flags & hdclaude::kCullAffectsPrimary) {
            scene.cullPrimaryVolumes |= 1u << bit;
        }
        if (volume.volume.flags & hdclaude::kCullAffectsSecondary) {
            scene.cullSecondaryVolumes |= 1u << bit;
        }
        if (volume.linksEverything) {
            cullLinksEverything |= 1u << bit;
            cullLinks.emplace_back();
        } else if (volume.cullLink.IsEmpty() || !_sceneIndex) {
            // Links nothing: an evaluator left empty never matches.
            cullLinks.emplace_back();
        } else {
            cullLinks.emplace_back(_sceneIndex, volume.cullLink);
        }
    }
    scene.cullVolumeCount = static_cast<std::uint32_t>(activeCullVolumes.size());
    // The membership of one Hydra prim, which every placement of it shares.
    // Instances of a native instance or a point instancer are not resolved
    // one by one, the limit HdsiLightLinkingSceneIndex has for light links.
    const auto cullLinksOf = [&](const SdfPath& path) -> std::uint32_t {
        std::uint32_t links = cullLinksEverything;
        for (std::size_t bit = 0; bit < cullLinks.size(); ++bit) {
            if (!cullLinks[bit].IsEmpty() && cullLinks[bit].Match(path)) {
                links |= 1u << bit;
            }
        }
        return links;
    };
    {
        std::vector<std::string> ignored;
        if (_cullVolumesEnabled) {
            std::size_t active = 0;
            for (const auto& [path, volume] : _cullVolumes) {
                if (volume.visible && selection.IncludesPurpose(volume.purpose) &&
                    ++active > hdclaude::kMaxCullVolumes) {
                    ignored.push_back(path.GetString());
                }
            }
        }
        const std::string overflow = TfStringJoin(ignored, ", ");
        if (overflow != _reportedCullOverflow) {
            if (!overflow.empty()) {
                TF_WARN("hdClaude: at most %u cull volumes are honoured, by "
                        "path; these are ignored: %s",
                        hdclaude::kMaxCullVolumes, overflow.c_str());
            }
            _reportedCullOverflow = overflow;
        }
    }

    // Light-linking categories, numbered by the lights that name them. A
    // category no light names cannot change anything, so geometry that
    // belongs only to those is linked to nothing and costs no membership bits.
    std::map<TfToken, std::int32_t> categoryIndex;
    const auto categoryOf = [&](const TfToken& token) -> std::int32_t {
        if (token.IsEmpty()) {
            return -1;
        }
        return categoryIndex
            .emplace(token, static_cast<std::int32_t>(categoryIndex.size()))
            .first->second;
    };
    for (const auto& [path, entry] : _lights) {
        categoryOf(entry.lightLink);
        categoryOf(entry.shadowLink);
    }
    scene.linkCategoryCount = static_cast<std::uint32_t>(categoryIndex.size());

    std::map<SdfPath, std::uint32_t> materialIndex;
    std::map<std::string, std::uint32_t> sharedIndex;
    for (const auto& [path, entry] : _materials) {
        // A material that cannot be used as authored -- no network hdClaude
        // reads (a glslfx- or RenderMan-only material), a surface that is not
        // MaterialX, a network that did not compile -- takes no index, and
        // resolves below as a binding to nothing.
        if (entry.compiled.spirv.empty() || !entry.fallbackReason.empty()) {
            continue;
        }
        // A material that compiled from the same network as one already in the
        // table *is* that material, texture slots included -- the pool keys an
        // image on its path -- so it takes the same index.
        if (!entry.shareKey.empty()) {
            const auto shared = sharedIndex.find(entry.shareKey);
            if (shared != sharedIndex.end()) {
                materialIndex[path] = shared->second;
                HdClaudeTrace("material %u: <%s> (same network)", shared->second,
                              path.GetText());
                continue;
            }
            sharedIndex.emplace(entry.shareKey,
                                static_cast<std::uint32_t>(materials.size()));
        }
        materialIndex[path] = static_cast<std::uint32_t>(materials.size());
        // The index a shading dispatch is keyed by, beside the prim it came
        // from. Nothing else in the renderer can map one to the other -- the
        // GPU knows only the index, and a diagnostic that reports one is
        // otherwise a number with no scene attached to it.
        HdClaudeTrace("material %u: <%s>", materialIndex[path], path.GetText());
        materials.push_back(entry.compiled);
    }

    const auto displayColorEntry =
        materialIndex.find(HdClaudeDisplayColorMaterialPath());
    const std::uint32_t displayColorIndex =
        displayColorEntry == materialIndex.end() ? 0u
                                                 : displayColorEntry->second;

    scene.prototypes.reserve(_meshes.size());
    for (const auto& [path, mesh] : _meshes) {
        if ((mesh.prototype.indices.empty() && mesh.prototype.segments.empty()) ||
            !mesh.visible || !selection.IncludesRprim(path)) {
            continue;
        }
        // A binding to no usable material is a binding to nothing: the prim
        // is shaded as an unbound one is, by its displayColor, and the
        // material's own report says why.
        auto resolve = [&](const SdfPath& binding) -> std::uint32_t {
            const auto found = materialIndex.find(binding);
            return found == materialIndex.end() ? displayColorIndex
                                                : found->second;
        };

        const auto prototype = static_cast<std::uint32_t>(scene.prototypes.size());
        scene.prototypes.push_back(mesh.prototype);

        const std::uint32_t material = resolve(mesh.material);

        // A material that samples an image on a mesh with no texture
        // coordinates.
        //
        // Every sample then reads the same texel, so a texture map is a flat
        // colour and a *displacement* map is a uniform swell -- the surface
        // keeps its shape and only changes size, which is indistinguishable
        // from displacement not working at all. Nothing else in the renderer
        // is in a position to notice: the mesh does not know what its material
        // samples and the material does not know what mesh it is bound to, and
        // this is the one place the two meet.
        //
        // Reported rather than worked around. A `UsdGeomSphere` is the case
        // that raised it and the coordinates are genuinely absent: OpenUSD's
        // implicit-surface scene index gives a generated sphere `points` and
        // nothing else, so it reaches every Hydra renderer this way, and
        // inventing a spherical projection here would be hdClaude answering a
        // question the scene did not ask. What fixes it is a `st` primvar on
        // the mesh, or a `texcoord`/`geompropvalue` node the material drives
        // its image with.
        if (material < materials.size() && !mesh.prototype.IsCurve() &&
            mesh.prototype.uvs.empty()) {
            const hdclaude::CompiledMaterial& bound = materials[material];
            const std::size_t images =
                bound.textureSlots.size() + bound.displaceTextureSlots.size();
            if (images > 0 && _reportedMissingUvs.insert(path).second) {
                TF_WARN(
                    "hdClaude: mesh <%s> has no texture coordinates, and its "
                    "material samples %zu image(s)%s. Every sample reads the "
                    "same texel, so a texture reads as a flat colour and a "
                    "displacement as a uniform change of size. Give the mesh "
                    "an `st` primvar, or drive the image from a `texcoord` or "
                    "`geompropvalue` node.",
                    path.GetText(), images,
                    bound.displaceTextureSlots.empty()
                        ? ""
                        : ", one of them for its displacement");
            }
        }

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

        // Where this mesh's instances were last time, if it was here last
        // time and had the same number of them. A mesh whose instance count
        // changed cannot be matched up one to one -- an instancer that grew or
        // shrank has no correspondence to offer -- so it reports no history
        // rather than pairing placements that are not the same placement.
        const auto previous = _previousTransforms.find(path);
        const bool matched = previous != _previousTransforms.end() &&
                             previous->second.size() == mesh.transforms.size();

        const std::uint32_t meshCullLinks = cullLinksOf(path);
        for (std::size_t i = 0; i < mesh.transforms.size(); ++i) {
            hdclaude::MeshInstance instance;
            instance.cullLinks = meshCullLinks;
            // The index into the flattened instances, the order
            // HdClaudeInstancer composes them in, which is the order Storm and
            // UsdImaging number them in; zero for a prim with no instancer.
            instance.pickId = mesh.primId;
            instance.pickInstance = static_cast<std::int32_t>(i);
            instance.prototype = prototype;
            instance.transform = mesh.transforms[i];
            if (matched) {
                instance.previousTransform = previous->second[i];
                instance.hasPreviousTransform = true;
            }
            instance.material = material;
            instance.visible = true;
            instance.rayVisibility = mesh.rayVisibility;
            std::copy(std::begin(mesh.displayColor), std::end(mesh.displayColor),
                      std::begin(instance.displayColor));
            if (i < mesh.instanceCategories.size() && !categoryIndex.empty()) {
                for (const TfToken& token : mesh.instanceCategories[i]) {
                    const auto found = categoryIndex.find(token);
                    if (found != categoryIndex.end()) {
                        instance.linkCategories.push_back(
                            static_cast<std::uint32_t>(found->second));
                    }
                }
            }
            scene.instances.push_back(std::move(instance));
        }
    }

    // Splat clouds. Their own prototype and instance lists: they build a
    // different kind of acceleration structure and are shaded by no material,
    // so nothing above resolves a binding for them.
    scene.splatPrototypes.reserve(_splats.size());
    for (const auto& [path, splats] : _splats) {
        if (!splats.prototype.cloud.Valid() || !splats.visible ||
            !selection.IncludesRprim(path)) {
            continue;
        }
        const auto prototype =
            static_cast<std::uint32_t>(scene.splatPrototypes.size());
        scene.splatPrototypes.push_back(splats.prototype);

        const auto previous = _previousSplatTransforms.find(path);
        const bool matched = previous != _previousSplatTransforms.end() &&
                             previous->second.size() == splats.transforms.size();

        const std::uint32_t splatCullLinks = cullLinksOf(path);
        for (std::size_t i = 0; i < splats.transforms.size(); ++i) {
            hdclaude::SplatInstance instance;
            instance.cullLinks = splatCullLinks;
            instance.pickId = splats.primId;
            instance.pickInstance = static_cast<std::int32_t>(i);
            instance.prototype = prototype;
            instance.transform = splats.transforms[i];
            if (matched) {
                instance.previousTransform = previous->second[i];
                instance.hasPreviousTransform = true;
            }
            instance.visible = true;
            instance.rayVisibility = splats.rayVisibility;
            if (i < splats.instanceCategories.size() && !categoryIndex.empty()) {
                for (const TfToken& token : splats.instanceCategories[i]) {
                    const auto found = categoryIndex.find(token);
                    if (found != categoryIndex.end()) {
                        instance.linkCategories.push_back(
                            static_cast<std::uint32_t>(found->second));
                    }
                }
            }
            scene.splatInstances.push_back(std::move(instance));
        }
    }

    // Lights, and the environment a dome light supplies.
    //
    // Several dome lights are legal in USD and rare in practice; their
    // radiances are summed, which is what a renderer that treated each as a
    // real emitter would arrive at, rather than silently honouring one.
    _snapshotReports.clear();
    const SdfPath* linkedDome = nullptr;

    // A light's tabulated illuminant, appended to the scene's table as
    // [n, wavelengths, values] and referred to by its offset, negated and
    // shifted so that zero stays D65 (Light::colorTemperature).
    const auto appendSpectrum = [&scene](const hdclaude::TabulatedSpectrum& spectrum) {
        const std::size_t offset = scene.emitterSpectra.size();
        scene.emitterSpectra.push_back(static_cast<float>(spectrum.wavelengths.size()));
        scene.emitterSpectra.insert(scene.emitterSpectra.end(),
                                    spectrum.wavelengths.begin(),
                                    spectrum.wavelengths.end());
        scene.emitterSpectra.insert(scene.emitterSpectra.end(),
                                    spectrum.values.begin(), spectrum.values.end());
        return -static_cast<float>(offset + 1);
    };

    // The environment has one illuminant, so the first dome supplies it, with
    // or without a map. It used to be taken only from a dome with a map, which
    // left a plain-coloured dome's colour temperature unread: such a dome
    // emitted D65 whatever it authored.
    const SdfPath* illuminantDome = nullptr;
    const HdClaudeLightEntry* illuminantEntry = nullptr;
    // Domes authored by the illuminance they deliver, held back from the
    // environment until it is known whether there is a map to integrate.
    std::vector<std::pair<const SdfPath*, const HdClaudeLightEntry*>> brightDomes;

    // The stage's metrics where the adapter published them, the settings
    // where it did not. A setting that was set and says otherwise is wrong
    // about this stage, and is said once a snapshot rather than once a light.
    bool reportedMeters = false;
    const auto metersFor = [&](const HdClaudeLightEntry& entry, const char** source) {
        const double stage = entry.brightness.stageMetersPerUnit;
        if (!(stage > 0.0)) {
            *source = "the metersPerUnit setting";
            return _metersPerUnit;
        }
        *source = "the stage's";
        if (_metersExplicit && !reportedMeters &&
            std::fabs(_metersPerUnit - stage) > 1e-6 * stage) {
            reportedMeters = true;
            _snapshotReports.push_back(TfStringPrintf(
                "the metersPerUnit render setting is %g and the stage's metersPerUnit "
                "is %g; lights authored in lumens or lux use the stage's",
                _metersPerUnit, stage));
            HdClaudeTrace("%s", _snapshotReports.back().c_str());
        }
        return stage;
    };
    for (const auto& [path, entry] : _lights) {
        if (entry.isDome) {
            // Several domes sum into one environment, and one environment has
            // one pair of links. Domes that agree are exact; domes that do not
            // cannot be represented, and the first one's links are used and
            // the disagreement said.
            if (linkedDome == nullptr) {
                linkedDome = &path;
                scene.domeLightLink = categoryOf(entry.lightLink);
                scene.domeShadowLink = categoryOf(entry.shadowLink);
            } else if (categoryOf(entry.lightLink) != scene.domeLightLink ||
                       categoryOf(entry.shadowLink) != scene.domeShadowLink) {
                _snapshotReports.push_back(
                    path.GetString() +
                    ": its light or shadow link differs from that of " +
                    linkedDome->GetString() +
                    ", and hdClaude sums dome lights into one environment with "
                    "one set of links, so that dome's links are used");
            }
            if (!scene.hasDomeLight) {
                scene.hasDomeLight = true;
                scene.environmentColor[0] = 0.0f;
                scene.environmentColor[1] = 0.0f;
                scene.environmentColor[2] = 0.0f;
            }
            if (entry.brightness.target == HdClaudeLightBrightness::Target::None) {
                for (int i = 0; i < 3; ++i) {
                    scene.environmentColor[i] += entry.environmentColor[i];
                }
            } else {
                brightDomes.emplace_back(&path, &entry);
            }
            if (illuminantEntry == nullptr) {
                illuminantDome = &path;
                illuminantEntry = &entry;
                scene.domeColorTemperature =
                    entry.emissionSpectrum.wavelengths.empty()
                        ? entry.domeColorTemperature
                        : appendSpectrum(entry.emissionSpectrum);
                scene.domeTemperatureScale = entry.domeTemperatureScale;
            } else if (entry.domeColorTemperature !=
                           illuminantEntry->domeColorTemperature ||
                       entry.domeTemperatureScale !=
                           illuminantEntry->domeTemperatureScale ||
                       entry.emissionSpectrum.wavelengths !=
                           illuminantEntry->emissionSpectrum.wavelengths ||
                       entry.emissionSpectrum.values !=
                           illuminantEntry->emissionSpectrum.values) {
                _snapshotReports.push_back(
                    path.GetString() + ": its illuminant differs from that of " +
                    illuminantDome->GetString() +
                    ", and hdClaude sums dome lights into one environment with "
                    "one illuminant, so that dome's illuminant is used");
            }
            // The first dome with a map supplies it. Two textured domes is not
            // a thing a renderer can composite meaningfully, so the second is
            // reported rather than silently blended.
            if (entry.domeTexture >= 0 && scene.domeTexture < 0) {
                scene.domeTexture = entry.domeTexture;
                std::copy(std::begin(entry.domeWorldToLight),
                          std::end(entry.domeWorldToLight),
                          std::begin(scene.domeWorldToLight));
                std::copy(std::begin(entry.domeLightToWorld),
                          std::end(entry.domeLightToWorld),
                          std::begin(scene.domeLightToWorld));
            }
        } else {
            hdclaude::Light light = entry.light;
            light.lightLink = categoryOf(entry.lightLink);
            light.shadowLink = categoryOf(entry.shadowLink);
            if (!entry.emissionSpectrum.wavelengths.empty()) {
                light.colorTemperature = appendSpectrum(entry.emissionSpectrum);
            }
            if (entry.brightness.target != HdClaudeLightBrightness::Target::None) {
                const char* source = "";
                const double metersPerUnit = metersFor(entry, &source);
                const double scale =
                    BrightnessScale(entry.brightness.value,
                                    RadianceLuminance(light.radiance),
                                    Delivery(entry, metersPerUnit));
                if (scale >= 0.0) {
                    for (float& channel : light.radiance) {
                        channel = static_cast<float>(channel * scale);
                    }
                    HdClaudeTrace("light <%s>: %s, at %g metres per unit (%s): "
                                  "luminance scaled by %g",
                                  path.GetText(), entry.brightness.statement.c_str(),
                                  metersPerUnit, source, scale);
                } else {
                    _snapshotReports.push_back(Unmet(path, entry.brightness.statement));
                    HdClaudeTrace("%s", _snapshotReports.back().c_str());
                }
            }
            scene.lights.push_back(light);
        }
    }

    // A dome's illuminance on an upward-facing surface is pi times its
    // luminance when it is uniform. With a map it is the map's cosine-weighted
    // integral over the upper hemisphere, which needs the map's texels: the
    // render pass finishes those once the snapshot has its textures.
    // Up is the stage's, from the first dome the adapter published it on --
    // the domes of one stage share it -- and the setting's otherwise.
    bool zUp = _zUp;
    for (const auto& [path, entry] : brightDomes) {
        const int stageUp = entry->brightness.stageUpAxis;
        if (stageUp < 0) {
            continue;
        }
        if (_upExplicit && (stageUp == 1) != _zUp) {
            _snapshotReports.push_back(TfStringPrintf(
                "the upAxis render setting is %s and the stage's upAxis is %s; a "
                "dome authored in lux uses the stage's",
                _zUp ? "Z" : "Y", stageUp == 1 ? "Z" : "Y"));
            HdClaudeTrace("%s", _snapshotReports.back().c_str());
        }
        zUp = stageUp == 1;
        break;
    }
    scene.upAxis[0] = 0.0;
    scene.upAxis[1] = zUp ? 0.0 : 1.0;
    scene.upAxis[2] = zUp ? 1.0 : 0.0;
    for (const auto& [path, entry] : brightDomes) {
        if (scene.domeTexture >= 0) {
            hdclaude::Scene::PendingDomeIlluminance pending;
            std::copy(std::begin(entry->environmentColor),
                      std::end(entry->environmentColor), std::begin(pending.color));
            pending.lux = entry->brightness.value;
            pending.statement = path->GetString() + ": " + entry->brightness.statement;
            scene.pendingDomeIlluminance.push_back(std::move(pending));
            continue;
        }
        const double scale =
            BrightnessScale(entry->brightness.value,
                            RadianceLuminance(entry->environmentColor), M_PI);
        if (scale < 0.0) {
            _snapshotReports.push_back(Unmet(*path, entry->brightness.statement));
            HdClaudeTrace("%s", _snapshotReports.back().c_str());
            continue;
        }
        for (int i = 0; i < 3; ++i) {
            scene.environmentColor[i] += static_cast<float>(entry->environmentColor[i] * scale);
        }
        HdClaudeTrace("dome light <%s>: %s: luminance scaled by %g", path->GetText(),
                      entry->brightness.statement.c_str(), scale);
    }

    // The stand-in sky is a *fallback*, and a stage that lights itself must not
    // get it.
    //
    // It only tinted escaping rays until the environment became an emitter
    // sampled by next-event estimation; from then on it was a fill light on
    // every scene, including the ones that author five lights of their own.
    // A stage with lights and no dome has no environment, and a ray that
    // leaves it sees nothing -- which is what every other renderer shows and
    // what the gallery's enclosed sets expect.
    if (!scene.hasDomeLight && !scene.lights.empty()) {
        scene.environmentColor[0] = 0.0f;
        scene.environmentColor[1] = 0.0f;
        scene.environmentColor[2] = 0.0f;
    }

    scene.hostInstanceData = _hostInstanceData;
    scene.tlasChunkSize = _tlasChunkSize;
    scene.revision = _revision;
    // This snapshot becomes the next one's past once a frame of it has been
    // rendered (CommitHistory), and not before. Recorded for every mesh
    // currently held, and only for those: a mesh that has gone should not
    // leave a history behind for a later mesh at the same path to inherit.
    _pendingTransforms.clear();
    for (const auto& [path, mesh] : _meshes) {
        _pendingTransforms[path] = mesh.transforms;
    }
    _pendingSplatTransforms.clear();
    for (const auto& [path, splats] : _splats) {
        _pendingSplatTransforms[path] = splats.transforms;
    }
    _historyPending = true;

    return scene;
}

void HdClaudeSceneStore::CommitHistory()
{
    std::lock_guard<std::mutex> lock(_mutex);
    if (!_historyPending) {
        return;
    }
    _previousTransforms = std::move(_pendingTransforms);
    _previousSplatTransforms = std::move(_pendingSplatTransforms);
    _pendingTransforms.clear();
    _pendingSplatTransforms.clear();
    _historyPending = false;
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
    for (const auto& [path, entry] : _splats) {
        for (const std::string& report : entry.reports) {
            reports.push_back(path.GetString() + ": " + report);
        }
    }
    reports.insert(reports.end(), _snapshotReports.begin(),
                   _snapshotReports.end());
    return reports;
}

PXR_NAMESPACE_CLOSE_SCOPE
