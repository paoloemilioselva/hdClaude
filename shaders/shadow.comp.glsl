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
    while (rayQueryProceedEXT(query)) { }

    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionNoneEXT)
    {
        // No atomic: a path emits at most one shadow ray per bounce, so no two
        // invocations here write the same path's radiance.
        pathRadiance.values[ray.path] += ray.contribution;
    }
}
