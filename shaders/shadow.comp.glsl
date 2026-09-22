#version 460
#extension GL_EXT_ray_query : require
#include "path_state.glsl"

// Batched shadow rays.
//
// Each record already carries the radiance it delivers, computed at shading
// time. This kernel only decides whether that contribution survives, which is
// what makes next-event estimation cheap: one dispatch for every light sample
// in the bounce, and no shading in it.

layout(local_size_x = 64) in;

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= counters.shadowCount)
    {
        return;
    }
    ShadowRay ray = shadowRays.values[index];

    // A light with a shadow link is occluded only by the geometry in it
    // (UsdLux `collection:shadowLink`). Membership is a property of the
    // instance, which only the shader can test, so for such a ray every
    // triangle is handed to the loop below as a candidate instead of being
    // committed by the implementation. Every other ray keeps the opaque fast
    // path.
    bool linked = ray.shadowLink >= 0;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT |
                 (linked ? gl_RayFlagsNoOpaqueEXT : gl_RayFlagsOpaqueEXT);

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas, flags, 0xFF, ray.origin, 1.0e-4,
                          ray.direction, ray.maxDistance);
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            // Only reached for a linked ray. The implementation has already
            // placed the hit inside the ray's interval.
            if (hdclaude_linked(
                    rayQueryGetIntersectionInstanceCustomIndexEXT(query, false),
                    ray.shadowLink))
            {
                rayQueryConfirmIntersectionEXT(query);
            }
            continue;
        }

        // A box is only ever a *candidate*. Triangle geometry is committed
        // by the implementation; a curve's box says "the segment inside me
        // might be hit", and this is where that question is answered.
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionAABBEXT)
        {
            int candidateInstance =
                rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
            InstanceGeometry candidateGeometry =
                instances.values[candidateInstance];
            if (candidateGeometry.splats != 0ul)
            {
                // A Gaussian splat occludes with the same probability it covers
                // a camera ray, drawn from the same hash of the same particle.
                // Over many samples that delivers the product of (1 - alpha)
                // along the ray, which is the transmittance the compositing
                // model implies -- and it comes out of independent coin flips
                // rather than an ordered walk, so nothing here sorts either.
                int candidateSplat =
                    rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
                Splat splat =
                    hdclaude_splat(candidateGeometry.splats, candidateSplat);

                vec3 candidateOrigin =
                    rayQueryGetIntersectionObjectRayOriginEXT(query, false);
                vec3 candidateDirection =
                    rayQueryGetIntersectionObjectRayDirectionEXT(query, false);

                // The search is bounded by the light here, and by nothing in
                // `extend`, and the difference is deliberate. A shadow ray
                // genuinely *ends* at the light, so a particle whose centre lies
                // beyond it is met only over the part of its support that is in
                // front, and the largest response the ray actually reaches is
                // the honest measure of that. A camera ray does not end at the
                // nearest hit found so far -- that hit can still be replaced --
                // so bounding its search the same way evaluates a particle's
                // falloff at another particle's depth, which is the defect the
                // two-particle composite caught.
                float hit;
                float response;
                if (hdclaude_splat_peak(splat, candidateGeometry.splatKernel,
                                        candidateOrigin, candidateDirection,
                                        rayQueryGetRayTMinEXT(query),
                                        ray.maxDistance, hit, response) &&
                    hdclaude_linked(candidateInstance, ray.shadowLink))
                {
                    float alpha = clamp(splat.opacity * response, 0.0, 1.0);
                    // A different stream from the camera ray's, because the two
                    // decide different questions about the same particle and
                    // sharing one would correlate a surface's shadow with what
                    // the camera sees through the cloud above it.
                    float coin = hdclaude_splat_coin(
                        ray.path + 0x9e3779b9u, frame.sampleIndex,
                        candidateInstance, candidateSplat);
                    if (coin < alpha)
                    {
                        rayQueryGenerateIntersectionEXT(query, hit);
                    }
                }
            }
            else if (candidateGeometry.segments != 0ul)
            {
                int candidateSegment =
                    rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
                SegmentBuffer curveSegments =
                    SegmentBuffer(candidateGeometry.segments);
                uint base = uint(candidateSegment) * 10u;
                vec3 pa = vec3(curveSegments.values[base + 0u],
                               curveSegments.values[base + 1u],
                               curveSegments.values[base + 2u]);
                float ra = curveSegments.values[base + 3u];
                vec3 pb = vec3(curveSegments.values[base + 5u],
                               curveSegments.values[base + 6u],
                               curveSegments.values[base + 7u]);
                float rb = curveSegments.values[base + 8u];

                // The object-space ray, which is the space the segments are
                // in. Asking the query for it rather than transforming the
                // world ray keeps the two from disagreeing about a
                // non-uniform scale.
                vec3 candidateOrigin =
                    rayQueryGetIntersectionObjectRayOriginEXT(query, false);
                vec3 candidateDirection =
                    rayQueryGetIntersectionObjectRayDirectionEXT(query, false);

                float hit = hdclaude_intersect_segment(
                    candidateOrigin, candidateDirection, pa, ra, pb, rb);
                // Inside the ray's own interval, at both ends. A triangle hit
                // is tested against it by the implementation; a *generated*
                // one is whatever the shader says it is, so the shader owes
                // the check. Below tMin an offset ray is stopped by the surface
                // it just left, which at a curve joint is a second segment
                // coincident with the first.
                //
                // Beyond the far end is the half that was missing. A shadow
                // ray stops at the light, but a curve's box can begin before
                // the light while the curve inside it lies past it, and that
                // hit was generated anyway. Vulkan's closest hit determination
                // says it is dropped; the driver this was measured on did not
                // drop it, and a comb of curves entirely beyond a light took
                // nine per cent of the light off the surface below it
                // (`tests/render_tests.cpp`). The comparison is made here
                // rather than trusted to the implementation.
                if (hit >= rayQueryGetRayTMinEXT(query) && hit < ray.maxDistance &&
                    hdclaude_linked(candidateInstance, ray.shadowLink))
                {
                    rayQueryGenerateIntersectionEXT(query, hit);
                }
            }
        }
    }

    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT)
    {
        // No atomic: a path emits at most one shadow ray per bounce, so no two
        // invocations here write the same path's radiance.
        pathRadiance.values[ray.path] += ray.contribution;
    }
}
