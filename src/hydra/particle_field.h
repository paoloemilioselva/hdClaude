#pragma once

#include "pxr/imaging/hd/rprim.h"

#include <string>
#include <vector>

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes a Hydra `particleField` rprim as a Gaussian splat cloud.
///
/// `UsdVolParticleField3DGaussianSplat` reaches a render delegate through
/// `UsdImagingParticleFieldAdapter`, which is registered for `ParticleField`
/// with `includeDerivedPrimTypes` and inserts an rprim of type
/// `HdPrimTypeTokens->particleField`. Hydra declares no `HdParticleField` base
/// class and no schema of its own for the data, so this derives from `HdRprim`
/// directly and reads the schema's attributes by name.
///
/// What it does *not* do is shade. A splat carries its radiance as spherical
/// harmonics, the schema has no reflectance in it at all, and a material bound
/// to the prim is therefore reported rather than applied. See
/// docs/gaussian-splats.md.
class HdClaudeParticleField final : public HdRprim {
  public:
    explicit HdClaudeParticleField(const SdfPath& id);
    ~HdClaudeParticleField() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    TfTokenVector const& GetBuiltinPrimvarNames() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits, const TfToken& reprToken) override;

    void Finalize(HdRenderParam* renderParam) override;

  protected:
    void _InitRepr(const TfToken& reprToken, HdDirtyBits* dirtyBits) override;
    HdDirtyBits _PropagateDirtyBits(HdDirtyBits bits) const override;

  private:
    /// Whether this prim has already had its data source described in the
    /// trace. Said once: a cloud that republishes every frame would otherwise
    /// bury the description in repetitions of itself.
    bool _described = false;

    /// What was last reported as not honoured as authored.
    ///
    /// Held so that the warnings are issued when they *change* rather than on
    /// every publication. Render stats carry these too, but a stats entry is
    /// invisible to `usdrecord`, and a report nobody reads is not a report --
    /// the negative-radiance case was diagnosed by hand before this existed.
    std::vector<std::string> _lastReports;
};

PXR_NAMESPACE_CLOSE_SCOPE
