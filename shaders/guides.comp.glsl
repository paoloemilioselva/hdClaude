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
        // A ray that hit nothing has no surface to have moved, so it reports no
        // motion. A reconstructor reprojecting the background uses the camera's
        // own motion, which it already knows.
        guideMotion.values[index] = vec2(0.0);
        // NVIDIA's sky defaults (DLSS-RR Integration Guide 3.4.1-3.4.4). An
        // analytic light is not geometry a material describes, so it keeps
        // them too.
        guideSurface.values[3u * index] = vec4(0.0);
        guideSurface.values[3u * index + 1u] = vec4(0.5, 0.5, 0.5, 0.0);
        guideSurface.values[3u * index + 2u] = vec4(0.0);
        guideSpecularRay.values[2u * index] = vec4(0.0);
        guideSpecularRay.values[2u * index + 1u] = vec4(0.0, 0.0, 0.0, 65504.0);
        return;
    }

    // Cleared for a hit as for a miss, and before anything below can return:
    // `shade` overwrites the surface guides and records a probe for a hit whose
    // material it runs, and a pixel it never reaches -- or a hit behind the
    // camera, which returns just below -- is not left holding a previous
    // frame's surface.
    guideSurface.values[3u * index] = vec4(0.0);
    guideSurface.values[3u * index + 1u] = vec4(0.5, 0.5, 0.5, 0.0);
    guideSurface.values[3u * index + 2u] = vec4(0.0);
    guideSpecularRay.values[2u * index] = vec4(0.0);
    guideSpecularRay.values[2u * index + 1u] = vec4(0.0, 0.0, 0.0, 65504.0);

    // `extend` leaves the hit point in the path's origin, so the position is
    // read rather than recomputed -- one fewer place for the intersection and
    // its consumer to disagree about where a hit was.
    vec3 hitWorld = pathOrigin.values[index];

    vec4 clip = frame.worldToClip * vec4(hitWorld, 1.0);
    if (clip.w <= 0.0)
    {
        guideDepth.values[index] = 1.0;
        guideMotion.values[index] = vec2(0.0);
        return;
    }

    // Where this surface was, and therefore where it moved from.
    //
    // The hit is taken back to the object it belongs to and forward again by
    // that object's previous placement, so an instance that moved contributes
    // its own motion and not just the camera's. `previousObjectToWorld` equals
    // `objectToWorld` for anything that did not move, which makes this the
    // identity there rather than a special case.
    //
    // Rigid only: a mesh whose points changed moved in a way no matrix
    // describes, and this reports the rigid part rather than pretending to the
    // rest.
    InstanceGeometry geometry = instances.values[record.x];
    vec3 objectPoint = vec4(hitWorld, 1.0) * geometry.worldToObject;
    vec3 previousWorld = vec4(objectPoint, 1.0) * geometry.previousObjectToWorld;
    vec4 previousClip = frame.previousWorldToClip * vec4(previousWorld, 1.0);

    // NDC z in [-1, 1] mapped to the [0, 1] the depth AOV is defined over,
    // matching hdEmbree, and clamped because a hit fractionally beyond the far
    // plane is at the far plane rather than outside the range.
    float ndc = clip.z / clip.w;
    guideDepth.values[index] = clamp((ndc + 1.0) * 0.5, 0.0, 1.0);

    // The motion, in pixels. Behind the previous camera there is no previous
    // pixel to point at, so the surface reports no motion rather than a
    // projection through the eye.
    if (previousClip.w <= 0.0)
    {
        guideMotion.values[index] = vec2(0.0);
        return;
    }
    vec2 nowNdc = clip.xy / clip.w;
    vec2 thenNdc = previousClip.xy / previousClip.w;
    // NDC spans [-1, 1] across the frame, so half the resolution converts a
    // difference in it to a difference in pixels.
    guideMotion.values[index] =
        (thenNdc - nowNdc) * vec2(frame.resolution) * 0.5;
}
