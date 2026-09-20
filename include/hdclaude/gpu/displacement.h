// Running a material's displacement program over a mesh.
//
// MaterialX's `displacement` terminal is a second program generated from the
// same document as the surface, and hdClaude's commitment
// (docs/architecture.md 7) is that it is *that program* which decides where
// the surface is -- compiled through the same `genglsl_pt` path and run as a
// GPU compute pass, never a CPU reimplementation of what the graph says.
//
// This is the pass. It owns one pipeline per compiled displacement program and
// dispatches it over a prototype's vertices, once per vertex, producing the
// object-space positions the acceleration structure is then built over.
//
// The result comes back to the host. That is a deliberate first answer to the
// residency question in docs/roadmap.md: a displaced prototype is an ordinary
// prototype, so it keeps the fingerprint, the reuse and the refit machinery
// exactly as it is, and a displacement that did not change costs no rebuild.
// The trade is a round trip that a device-resident vertex buffer would not
// pay, and it is a measurement to make rather than a guess -- the pass runs at
// scene publication, not in a frame.

#ifndef HDCLAUDE_GPU_DISPLACEMENT_H
#define HDCLAUDE_GPU_DISPLACEMENT_H

#include <cstdint>
#include <string>
#include <vector>

#include <volk.h>

#include "hdclaude/gpu/compute_pipeline.h"
#include "hdclaude/gpu/scene.h"
#include "hdclaude/gpu/vertex_frames.h"
#include "hdclaude/gpu/vulkan_context.h"
#include "hdclaude/gpu/vulkan_resources.h"

namespace hdclaude {

/// The descriptor interface a displacement pipeline declares.
///
/// One binding: the shared texture array, at the same slot the shade kernel's
/// layout gives it, because the *generated material* declares that slot and
/// the same generated code is joined to either kernel. Everything else a
/// displacement reads -- positions, normals, derivatives, texture coordinates
/// -- arrives as a device address in a push constant, because a displacement
/// runs once per prototype rather than once per frame and there is no
/// per-frame descriptor set to put a prototype's buffers in.
std::vector<BindingDescription> DisplacementBindings();

/// One compiled displacement program, ready to run over meshes.
class DisplacementPass {
  public:
    DisplacementPass() = default;

    /// `spirv` is a generated displacement program joined to
    /// `shaders/displace.comp.glsl`; `space` is what the host read from the
    /// document, because the generated code cannot carry it.
    DisplacementPass(const VulkanContext& context,
                     const std::vector<std::uint32_t>& spirv,
                     DisplacementSpace space, std::string debugName);

    bool Valid() const { return _pipeline.Valid(); }
    DisplacementSpace Space() const { return _space; }
    const std::string& DebugName() const { return _debugName; }

    /// Point this pass at the images its material samples.
    ///
    /// Every slot of the array is written, including the unused tail: a
    /// descriptor that is declared and never written is undefined the moment
    /// generated code indexes it. A pass whose material samples nothing still
    /// needs the call, with the placeholder in every slot.
    void SetTextures(const std::vector<VkDescriptorImageInfo>& textures);

    /// Displace `prototype` and return its object-space positions.
    ///
    /// `frames` must be `ComputeVertexFrames(prototype)`; it is taken rather
    /// than computed here so a caller displacing one prototype with several
    /// materials pays for the frame once.
    ///
    /// Returns an empty vector, having done nothing, when the prototype is a
    /// curve, when the frames do not describe it, or when this pass is not
    /// valid. A caller that gets nothing back keeps the positions it had,
    /// which is the undisplaced mesh rather than no mesh.
    std::vector<float> Displace(VulkanAllocator& allocator,
                                const MeshPrototype& prototype,
                                const VertexFrames& frames) const;

  private:
    const VulkanContext* _context = nullptr;
    ComputePipeline _pipeline;
    VkDescriptorSet _set = VK_NULL_HANDLE;
    DisplacementSpace _space = DisplacementSpace::AlongNormal;
    std::string _debugName;
};

}  // namespace hdclaude

#endif  // HDCLAUDE_GPU_DISPLACEMENT_H
