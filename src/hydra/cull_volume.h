#pragma once

#include "pxr/imaging/hd/sprim.h"

PXR_NAMESPACE_OPEN_SCOPE

/// Publishes a cull volume (extensions/cullVolume) to the scene store.
///
/// The Hydra prim type is `cullVolume`, published by the usdImagingCullVolume
/// adapter. hdClaude does not link that library: the prim's data is read by
/// name from the terminal scene index, because the emulated scene delegate
/// answers `Get` for no key of a prim type it does not know
/// (docs/cull-volumes.md 3.2).
///
/// What Sync publishes decides what a change costs. The transform and the
/// shape are uploaded on their own, so moving a volume rebuilds nothing; which
/// rays it affects, whether it is active, and which geometry it links change
/// the scene's instance masks and so take a scene publication.
class HdClaudeCullVolume final : public HdSprim {
  public:
    explicit HdClaudeCullVolume(const SdfPath& id);
    ~HdClaudeCullVolume() override;

    HdDirtyBits GetInitialDirtyBitsMask() const override;

    void Sync(HdSceneDelegate* sceneDelegate, HdRenderParam* renderParam,
              HdDirtyBits* dirtyBits) override;

    void Finalize(HdRenderParam* renderParam) override;

  private:
    /// The last thing reported about this prim, so a report is made when it
    /// changes rather than on every Sync of an animated volume.
    std::string _lastReport;
};

PXR_NAMESPACE_CLOSE_SCOPE
