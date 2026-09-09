#version 460
#include "path_state.glsl"

// The guide buffers, written from the primary hit.
//
// Dispatched once per sample, immediately after the first bounce's `extend`,
// which is where the primary hit exists and nowhere else: `extend` has already
// moved each path's origin onto its hit point and recorded what it hit, and the
// bounce after this one overwrites both.
//
// A separate kernel rather than a branch inside `extend`, because the condition
// is *which bounce this is* and the host already knows -- it decides the
// dispatch. Putting it inside `extend` would hand that kernel a bounce index it
// has no other use for.

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= frame.resolution.x || pixel.y >= frame.resolution.y)
    {
        return;
    }
    uint index = pixel.y * frame.resolution.x + pixel.x;

    // At the first bounce a path's index is its pixel: raygen fills the queue
    // densely and compaction has not run yet.
    ivec4 record = hits.values[index];
    if (record.x < 0)
    {
        // The clear value Hydra gives a depth AOV, which is the far plane. A
        // ray that hit nothing is not at zero depth, it is at no depth, and 1.0
        // is how that is spelled.
        guideDepth.values[index] = 1.0;
        return;
    }

    // `extend` leaves the hit point in the path's origin, so the position is
    // read rather than recomputed -- one fewer place for the intersection and
    // its consumer to disagree about where a hit was.
    vec3 hitWorld = pathOrigin.values[index];

    vec4 clip = frame.worldToClip * vec4(hitWorld, 1.0);
    if (clip.w <= 0.0)
    {
        guideDepth.values[index] = 1.0;
        return;
    }

    // NDC z in [-1, 1] mapped to the [0, 1] the depth AOV is defined over,
    // matching hdEmbree, and clamped because a hit fractionally beyond the far
    // plane is at the far plane rather than outside the range.
    float ndc = clip.z / clip.w;
    guideDepth.values[index] = clamp((ndc + 1.0) * 0.5, 0.0, 1.0);
}
