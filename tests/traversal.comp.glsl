#version 460
#extension GL_EXT_ray_query : require
#extension GL_EXT_scalar_block_layout : require

// Traversal check: fire a grid of rays at the scene and record what they hit.
//
// This is the smallest program that exercises the whole geometry path -- upload,
// bottom-level build, instance transforms, top-level build, and ray query --
// and it reports enough to tell a miss from a wrong hit. A test that only
// counted hits would pass on a structure built at the wrong scale or with the
// instance transform transposed.

layout(local_size_x = 8, local_size_y = 8) in;

layout(set = 0, binding = 0) uniform accelerationStructureEXT scene;

layout(set = 0, binding = 1, std430) buffer Results {
    // Per pixel, so the host can compare an expected image rather than a total.
    //   x  instance custom index, or -1 for a miss
    //   y  primitive index
    //   z  barycentric u, fixed point x 65536
    //   w  hit distance, fixed point x 65536
    ivec4 hits[];
} results;

layout(push_constant) uniform Params {
    uint width;
    uint height;
    float orthoHalfExtent;   // half-width of the orthographic view, world units
    float rayOriginZ;
} params;

void main()
{
    uint x = gl_GlobalInvocationID.x;
    uint y = gl_GlobalInvocationID.y;
    if (x >= params.width || y >= params.height)
    {
        return;
    }
    uint index = y * params.width + x;

    // Orthographic rays down -Z. Deterministic and trivially invertible, so an
    // expected hit pattern can be computed on the host in closed form.
    vec2 uv = (vec2(x, y) + 0.5) / vec2(params.width, params.height);
    vec2 plane = (uv * 2.0 - 1.0) * params.orthoHalfExtent;

    vec3 origin = vec3(plane.x, plane.y, params.rayOriginZ);
    vec3 direction = vec3(0.0, 0.0, -1.0);

    rayQueryEXT query;
    rayQueryInitializeEXT(query, scene, gl_RayFlagsOpaqueEXT, 0xFF, origin,
                          0.0, direction, 1000.0);
    while (rayQueryProceedEXT(query)) { }

    ivec4 result = ivec4(-1, -1, 0, 0);
    if (rayQueryGetIntersectionTypeEXT(query, true) ==
        gl_RayQueryCommittedIntersectionTriangleEXT)
    {
        result.x = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
        result.y = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        vec2 bary = rayQueryGetIntersectionBarycentricsEXT(query, true);
        result.z = int(bary.x * 65536.0);
        result.w = int(rayQueryGetIntersectionTEXT(query, true) * 65536.0);
    }
    results.hits[index] = result;
}
