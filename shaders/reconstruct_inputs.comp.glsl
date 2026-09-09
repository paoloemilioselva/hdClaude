#version 460
#extension GL_EXT_scalar_block_layout : require

// The reconstruction backend's inputs, moved from buffers into images.
//
// A backend reads textures: DLSS takes VkImage handles and samples them, and
// the wavefront integrator's film and guides live in storage buffers because
// every other kernel indexes them by path. This kernel is the one place those
// two facts meet, and it exists rather than having `film` and `guides` write
// images directly for a reason worth stating: a reference render must be
// bit-identical whether or not a backend is present (docs/dlss-integration.md
// 5), and the surest way to keep it so is for the kernels that produce it never
// to learn that reconstruction exists.
//
// It is dispatched only for an interactive frame that is being reconstructed,
// so a reference render pays nothing for it -- not a dispatch, not a barrier,
// and not an image allocation.
//
// The divide is the same one the host does when it resolves the film: the
// accumulation holds a sum in `rgb` and the number of samples that reached the
// pixel in `w`. Doing it here rather than reading the film back and uploading
// it again is the whole point of the kernel.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0, scalar) readonly buffer Accumulation {
    vec4 values[];
} accumulation;
layout(set = 0, binding = 1, scalar) readonly buffer GuideDepth {
    float values[];
} guideDepth;
layout(set = 0, binding = 2, scalar) readonly buffer GuideMotion {
    vec2 values[];
} guideMotion;

layout(set = 0, binding = 3, rgba16f) uniform writeonly image2D colorImage;
layout(set = 0, binding = 4, r32f) uniform writeonly image2D depthImage;
layout(set = 0, binding = 5, rg16f) uniform writeonly image2D motionImage;

layout(push_constant) uniform Params {
    uvec2 extent;
} params;

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= params.extent.x || pixel.y >= params.extent.y)
    {
        return;
    }
    uint index = pixel.y * params.extent.x + pixel.x;

    // Row 0 of the image is row 0 of the buffer, which is the bottom of the
    // frame. Colour, depth and motion are all written this way round, and the
    // motion vectors are in the same axis directions as these rows, so the
    // whole of what a temporal filter reprojects within is one consistent
    // coordinate system -- which is all it needs (see the contract on
    // ReconstructionFrame in include/hdclaude/gpu/reconstruction.h).
    ivec2 coordinate = ivec2(pixel);

    vec4 film = accumulation.values[index];
    vec3 colour = film.w > 0.0 ? film.rgb / film.w : vec3(0.0);

    // Not clamped, and not sanitised. A non-finite pixel is a defect in the
    // estimator, and a backend that receives one should produce visible damage
    // rather than have this kernel quietly repair it
    // (docs/dlss-integration.md 8).
    imageStore(colorImage, coordinate, vec4(colour, 1.0));
    imageStore(depthImage, coordinate, vec4(guideDepth.values[index], 0.0, 0.0, 0.0));
    imageStore(motionImage, coordinate, vec4(guideMotion.values[index], 0.0, 0.0));
}
