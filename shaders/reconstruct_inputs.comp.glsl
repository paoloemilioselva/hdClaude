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
//
// It also reduces the frame's luminance, one partial sum per workgroup, because
// it is already reading every pixel and DLSS needs the frame's average
// luminance to be told what its exposure is (docs/dlss-integration.md 5b).
// `reconstruct_exposure.comp.glsl` turns these partials into that value.

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

layout(set = 0, binding = 6, scalar) writeonly buffer LuminancePartials {
    float values[];
} partials;

// The primary surface's guides, three vec4 per pixel -- normal and roughness,
// diffuse albedo, specular albedo -- and the images Ray Reconstruction reads
// them from. Packed for Super Resolution too, which ignores them: one kernel and
// one set of images is simpler than two, at three render-extent images.
layout(set = 0, binding = 7, scalar) readonly buffer GuideSurface {
    vec4 values[];
} guideSurface;
layout(set = 0, binding = 8, rgba16f) uniform writeonly image2D normalRoughnessImage;
layout(set = 0, binding = 9, rgba16f) uniform writeonly image2D diffuseAlbedoImage;
layout(set = 0, binding = 10, rgba16f) uniform writeonly image2D specularAlbedoImage;
layout(set = 0, binding = 11, scalar) readonly buffer GuideSpecularRay {
    vec4 values[];
} guideSpecularRay;
layout(set = 0, binding = 12, r32f) uniform writeonly image2D specularHitDistanceImage;

layout(push_constant) uniform Params {
    uvec2 extent;
} params;

// One entry per invocation of the workgroup. The reduction is in shared memory
// rather than through an atomic because a float atomic needs
// VK_EXT_shader_atomic_float, which nothing else here requires.
shared float workgroupLuminance[64];

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    // Deliberately *not* an early return.
    //
    // Every invocation of this workgroup has to reach the barriers below, and a
    // pixel outside the image would otherwise skip them -- which is undefined
    // behaviour, and the kind that works until the extent stops being a
    // multiple of the workgroup size. An out-of-range invocation contributes
    // zero and stores nothing.
    const bool inside = pixel.x < params.extent.x && pixel.y < params.extent.y;

    float luminance = 0.0;

    if (inside)
    {
        uint index = pixel.y * params.extent.x + pixel.x;

        // Row 0 of the image is row 0 of the buffer, which is the bottom of the
        // frame. Colour, depth and motion are all written this way round, and
        // the motion vectors are in the same axis directions as these rows, so
        // the whole of what a temporal filter reprojects within is one
        // consistent coordinate system -- which is all it needs (see the
        // contract on ReconstructionFrame in
        // include/hdclaude/gpu/reconstruction.h).
        ivec2 coordinate = ivec2(pixel);

        vec4 film = accumulation.values[index];
        vec3 colour = film.w > 0.0 ? film.rgb / film.w : vec3(0.0);

        // Not clamped, and not sanitised. A non-finite pixel is a defect in the
        // estimator, and a backend that receives one should produce visible
        // damage rather than have this kernel quietly repair it
        // (docs/dlss-integration.md 8).
        imageStore(colorImage, coordinate, vec4(colour, 1.0));
        imageStore(depthImage, coordinate, vec4(guideDepth.values[index], 0.0, 0.0, 0.0));
        imageStore(motionImage, coordinate, vec4(guideMotion.values[index], 0.0, 0.0));
        imageStore(normalRoughnessImage, coordinate, guideSurface.values[3u * index]);
        imageStore(diffuseAlbedoImage, coordinate,
                   vec4(guideSurface.values[3u * index + 1u].rgb, 1.0));
        imageStore(specularAlbedoImage, coordinate,
                   vec4(guideSurface.values[3u * index + 2u].rgb, 1.0));
        imageStore(specularHitDistanceImage, coordinate,
                   vec4(guideSpecularRay.values[2u * index + 1u].w, 0.0, 0.0, 0.0));

        // Rec.709 on the linear colour actually handed over, which is what the
        // exposure has to be an exposure *of*. A non-finite pixel is excluded
        // from the sum rather than allowed to make the whole frame's exposure
        // non-finite -- one broken pixel is a visible defect, and one broken
        // exposure is an entirely black or white image that hides it.
        const float pixelLuminance =
            0.2126 * colour.r + 0.7152 * colour.g + 0.0722 * colour.b;
        luminance = isinf(pixelLuminance) || isnan(pixelLuminance)
                        ? 0.0
                        : max(pixelLuminance, 0.0);
    }

    const uint lane = gl_LocalInvocationIndex;
    workgroupLuminance[lane] = luminance;
    barrier();

    // A tree reduction over the 64 invocations. Every invocation reaches every
    // barrier, including the ones whose pixel fell outside the image.
    for (uint stride = 32u; stride > 0u; stride >>= 1)
    {
        if (lane < stride)
        {
            workgroupLuminance[lane] += workgroupLuminance[lane + stride];
        }
        barrier();
    }

    if (lane == 0u)
    {
        const uint groupIndex =
            gl_WorkGroupID.y * gl_NumWorkGroups.x + gl_WorkGroupID.x;
        partials.values[groupIndex] = workgroupLuminance[0];
    }
}
