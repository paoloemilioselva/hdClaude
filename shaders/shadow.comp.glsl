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

// Whether anything occludes the shadow ray over [tMin, tMax], among the
// placements whose TLAS mask shares a bit with `cullMask`.
//
// With `exceptions`, a hit also has to survive the cull volumes among
// `volumes` that link the hit placement (docs/cull-volumes.md 2.1). Without
// it, the stretch is one the volumes already keep for every placement it can
// meet.
bool shadow_occluded(ShadowRay ray, float tMin, float tMax, uint cullMask,
                     bool exceptions, uint volumes)
{
    // A light with a shadow link is occluded only by the geometry in it
    // (UsdLux `collection:shadowLink`). Membership is a property of the
    // instance, which only the shader can test, so for such a ray every
    // triangle is handed to the loop below as a candidate instead of being
    // committed by the implementation. Every other ray keeps the opaque fast
    // path.
    //
    // The exception query of a cull volume hands every triangle to the loop for
    // the same reason: whether the hit placement is culled there is a property of
    // the placement.
    bool linked = ray.shadowLink >= 0;
    uint flags = gl_RayFlagsTerminateOnFirstHitEXT |
                 (linked || exceptions ? gl_RayFlagsNoOpaqueEXT
                                       : gl_RayFlagsOpaqueEXT);

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas, flags, cullMask, ray.origin, tMin,
                          ray.direction, tMax);
    while (rayQueryProceedEXT(query))
    {
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            // Only reached for a linked ray or an exception query. The
            // implementation has already placed the hit inside the ray's
            // interval.
            int candidateInstance =
                rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
            if (hdclaude_linked(candidateInstance, ray.shadowLink) &&
                !(exceptions &&
                  hdclaude_cull_point(
                      rayQueryGetIntersectionTEXT(query, false),
                      instanceCullLinks.values[candidateInstance] & volumes)))
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
            if (candidateGeometry.splats != 0ul &&
                frame.splatTransport == HDCLAUDE_SPLAT_COVERAGE)
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
                                        tMax, hit, response) &&
                    hdclaude_linked(candidateInstance, ray.shadowLink) &&
                    !(exceptions &&
                      hdclaude_cull_point(
                          hit, instanceCullLinks.values[candidateInstance] &
                                   volumes)))
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
                if (hit >= rayQueryGetRayTMinEXT(query) && hit < tMax &&
                    hdclaude_linked(candidateInstance, ray.shadowLink) &&
                    !(exceptions &&
                      hdclaude_cull_point(
                          hit, instanceCullLinks.values[candidateInstance] &
                                   volumes)))
                {
                    rayQueryGenerateIntersectionEXT(query, hit);
                }
            }
        }
    }

    return rayQueryGetIntersectionTypeEXT(query, true) !=
           gl_RayQueryCommittedIntersectionNoneEXT;
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= counters.shadowCount)
    {
        return;
    }
    ShadowRay ray = shadowRays.values[index];

    // A shadow ray is a secondary ray, so it is traced in the stretches the
    // cull volumes affecting secondary rays keep, then once more against the
    // exception placements (docs/cull-volumes.md 2).
    uint volumes = hdclaude_cull_volumes(false);
    bool occluded = false;
    if (volumes == 0u)
    {
        occluded = shadow_occluded(ray, 1.0e-4, ray.maxDistance, 0xFFu, false,
                                   0u);
    }
    else
    {
        hdclaude_cull_prepare(ray.origin, ray.direction, volumes);
        float t = 1.0e-4;
        float stretchEnd;
        for (uint guard = 0u;
             !occluded && guard <= HDCLAUDE_MAX_CULL_VOLUMES &&
             hdclaude_cull_next(t, ray.maxDistance, volumes, stretchEnd);
             ++guard)
        {
            occluded = shadow_occluded(ray, t, stretchEnd, HDCLAUDE_MASK_SECONDARY,
                                       false, volumes);
            t = stretchEnd;
        }
        if (!occluded && (frame.cullExceptions & 2u) != 0u)
        {
            occluded = shadow_occluded(ray, 1.0e-4, ray.maxDistance,
                                       HDCLAUDE_MASK_SECONDARY_EXCEPTION, true,
                                       volumes);
        }
    }

    // A splat cloud read as a medium occludes by the same walk that stops a
    // camera ray. A real collision anywhere along the shadow ray blocks it, and
    // over many samples that delivers the cloud's transmittance -- the same
    // quantity the coverage model's independent coin flips deliver, arrived at
    // by the reading that is in force.
    bool blockedByVolume = false;
    if (frame.splatTransport == HDCLAUDE_SPLAT_VOLUME &&
        frame.splatInstanceCount > 0u)
    {
        uint rng = pathRng.values[ray.path] ^ 0x5bf03635u;
        for (uint i = 0u; i < frame.splatInstanceCount && !blockedByVolume; ++i)
        {
            InstanceGeometry cloud =
                instances.values[frame.splatInstanceBegin + i];
            if (cloud.splats == 0ul || cloud.splatVolume == 0ul ||
                !hdclaude_linked(int(frame.splatInstanceBegin + i),
                                 ray.shadowLink))
            {
                continue;
            }
            SplatVolume volume = SplatVolumeBuffer(cloud.splatVolume).value;
            float collision;
            vec3 emitted;
            // Only over the stretches this cloud's cull volumes keep, as in
            // `extend`.
            uint cloudVolumes =
                volumes & instanceCullLinks.values[frame.splatInstanceBegin + i];
            if (cloudVolumes == 0u)
            {
                blockedByVolume = hdclaude_splat_volume_collision(
                    cloud, volume, ray.origin, ray.direction, 0.0,
                    ray.maxDistance, rng, collision, emitted);
            }
            else
            {
                float t = 0.0;
                float stretchEnd;
                for (uint guard = 0u;
                     !blockedByVolume && guard <= HDCLAUDE_MAX_CULL_VOLUMES &&
                     hdclaude_cull_next(t, ray.maxDistance, cloudVolumes,
                                        stretchEnd);
                     ++guard)
                {
                    blockedByVolume = hdclaude_splat_volume_collision(
                        cloud, volume, ray.origin, ray.direction, t, stretchEnd,
                        rng, collision, emitted);
                    t = stretchEnd;
                }
            }
        }
    }

    if (!blockedByVolume && !occluded)
    {
        // No atomic: a path emits at most one shadow ray per bounce, so no two
        // invocations here write the same path's radiance.
        pathRadiance.values[ray.path] += ray.contribution;
    }
}
