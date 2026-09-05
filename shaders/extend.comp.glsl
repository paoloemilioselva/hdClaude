#version 460
#extension GL_EXT_ray_query : require
#include "path_state.glsl"

// Closest-hit traversal for the active queue.
//
// Writes a hit record and nothing else. Keeping shading out of this kernel is
// what lets the next one be dispatched per material, which is the whole reason
// the integrator is wavefront (docs/architecture.md 2).

layout(local_size_x = 64) in;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];

    vec3 origin = pathOrigin.values[path];
    vec3 direction = pathDirection.values[path];

    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFF,
                          origin, 0.0, direction, 1.0e30);
    while (rayQueryProceedEXT(query)) { }

    ivec4 record = ivec4(-1, -1, 0, 0);
    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionTriangleEXT)
    {
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
        record.x = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
        record.y = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        record.z = floatBitsToInt(bary.x);
        record.w = floatBitsToInt(bary.y);

        // The hit position replaces the origin, so shading needs no ray
        // parameter and no second evaluation of origin + t * direction.
        pathOrigin.values[path] =
            origin + direction * rayQueryGetIntersectionTEXT(query, true);
    }
    hits.values[path] = record;
}
