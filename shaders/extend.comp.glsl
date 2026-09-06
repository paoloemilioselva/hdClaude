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
    float tGeometry = 1.0e30;
    bool hitGeometry = rayQueryGetIntersectionTypeEXT(query, true) ==
                       gl_RayQueryCommittedIntersectionTriangleEXT;
    if (hitGeometry)
    {
        tGeometry = rayQueryGetIntersectionTEXT(query, true);
    }

    // An analytic light is intersected in closed form and competes with the
    // geometry hit on distance, which is what makes it an opaque emitter: a
    // light in front of a wall hides the wall, and a light behind it does not
    // shine through.
    float tLight;
    vec3 lightNormal;
    int light = hdclaude_nearest_light(origin, direction, tGeometry, tLight,
                                       lightNormal);

    if (light >= 0)
    {
        // Encoded below -1 so the sort skips it exactly as it skips a miss,
        // and the kernel that retires missed paths adds the emission. The
        // origin is deliberately *not* advanced: that kernel re-intersects the
        // light to recover the distance and normal the density needs, which
        // costs one intersection and no extra path state.
        record.x = -2 - light;
    }
    else if (hitGeometry)
    {
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
        record.x = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
        record.y = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        record.z = floatBitsToInt(bary.x);
        record.w = floatBitsToInt(bary.y);

        // The hit position replaces the origin, so shading needs no ray
        // parameter and no second evaluation of origin + t * direction.
        pathOrigin.values[path] = origin + direction * tGeometry;
    }
    hits.values[path] = record;
}
