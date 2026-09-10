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

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas,
                          gl_RayFlagsOpaqueEXT | gl_RayFlagsTerminateOnFirstHitEXT,
                          0xFF, ray.origin, 1.0e-4, ray.direction,
                          ray.maxDistance);
    while (rayQueryProceedEXT(query))
    {
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
            if (candidateGeometry.segments != 0ul)
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
                if (hit > 0.0)
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
