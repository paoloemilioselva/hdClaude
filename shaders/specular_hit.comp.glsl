#version 460
#extension GL_EXT_ray_query : require
#include "path_state.glsl"

// The specular hit distance guide, from probe rays `shade` recorded.
//
// DLSS Ray Reconstruction defines it as "the World Space distance between the
// Specular Ray Origin and Hit Point", the origin "on the Primary Surface"
// (Integration Guide 3.4.9), and says nothing of which specular ray: a path
// tracer samples one lobe per path, so the ray its path actually took is
// diffuse at many pixels and noisy at every one, and the guide warns that noise
// in a guide confuses the reconstruction (3.5). NRD, NVIDIA's denoiser before
// it, takes hit distance "from surfaces that are in the specular reflection
// lobe". So the ray here is the one the specular lobe is centred on -- the view
// reflected about the shading normal the closures answer to -- traced once per
// pixel, and the distance is what it meets. Recorded as a decision in the
// roadmap, because the specification does not make it.
//
// Dispatched once per sample after the first bounce is shaded, over pixels:
// `shade` wrote the probe for every primary hit it reached, and `guides` left
// every other pixel holding no probe and NVIDIA's sky value.

layout(local_size_x = 8, local_size_y = 8) in;

// FP16_MAX, the guide's value for a ray that meets nothing.
const float kNoHit = 65504.0;

// The nearest surface over [tMin, tMax] among the placements whose TLAS mask
// shares a bit with `cullMask`, into `tGeometry` when there is one. With
// `exceptions`, a hit must also survive the placement's own cull volumes among
// `volumes`, as in `extend`.
void specular_trace(vec3 origin, vec3 direction, float tMin, float tMax,
                    uint cullMask, bool exceptions, uint volumes,
                    inout float tGeometry)
{
    rayQueryEXT query;
    rayQueryInitializeEXT(query, sceneTlas,
                          exceptions ? gl_RayFlagsNoOpaqueEXT
                                     : gl_RayFlagsOpaqueEXT,
                          cullMask, origin, tMin, direction, tMax);
    while (rayQueryProceedEXT(query))
    {
        int candidateInstance =
            rayQueryGetIntersectionInstanceCustomIndexEXT(query, false);
        // Only reached by the exception query; see extend.comp.glsl.
        if (rayQueryGetIntersectionTypeEXT(query, false) ==
            gl_RayQueryCandidateIntersectionTriangleEXT)
        {
            if (!hdclaude_cull_point(
                    rayQueryGetIntersectionTEXT(query, false),
                    instanceCullLinks.values[candidateInstance] & volumes))
            {
                rayQueryConfirmIntersectionEXT(query);
            }
            continue;
        }
        if (rayQueryGetIntersectionTypeEXT(query, false) !=
            gl_RayQueryCandidateIntersectionAABBEXT)
        {
            continue;
        }
        InstanceGeometry candidateGeometry = instances.values[candidateInstance];
        if (candidateGeometry.segments == 0ul)
        {
            continue;
        }
        int candidateSegment = rayQueryGetIntersectionPrimitiveIndexEXT(query, false);
        SegmentBuffer curveSegments = SegmentBuffer(candidateGeometry.segments);
        uint base = uint(candidateSegment) * 10u;
        vec3 pa = vec3(curveSegments.values[base + 0u],
                       curveSegments.values[base + 1u],
                       curveSegments.values[base + 2u]);
        float ra = curveSegments.values[base + 3u];
        vec3 pb = vec3(curveSegments.values[base + 5u],
                       curveSegments.values[base + 6u],
                       curveSegments.values[base + 7u]);
        float rb = curveSegments.values[base + 8u];
        float hit = hdclaude_intersect_segment(
            rayQueryGetIntersectionObjectRayOriginEXT(query, false),
            rayQueryGetIntersectionObjectRayDirectionEXT(query, false), pa, ra,
            pb, rb);
        float committedT =
            rayQueryGetIntersectionTypeEXT(query, true) !=
                    gl_RayQueryCommittedIntersectionNoneEXT
                ? rayQueryGetIntersectionTEXT(query, true)
                : 1.0e30;
        if (hit >= rayQueryGetRayTMinEXT(query) && hit < min(committedT, tMax) &&
            !(exceptions &&
              hdclaude_cull_point(
                  hit, instanceCullLinks.values[candidateInstance] & volumes)))
        {
            rayQueryGenerateIntersectionEXT(query, hit);
        }
    }

    uint committed = rayQueryGetIntersectionTypeEXT(query, true);
    if (committed == gl_RayQueryCommittedIntersectionTriangleEXT ||
        committed == gl_RayQueryCommittedIntersectionGeneratedEXT)
    {
        tGeometry = rayQueryGetIntersectionTEXT(query, true);
    }
}

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= frame.resolution.x || pixel.y >= frame.resolution.y)
    {
        return;
    }
    uint index = pixel.y * frame.resolution.x + pixel.x;

    vec4 origin = guideSpecularRay.values[2u * index];
    if (origin.w < 0.5)
    {
        return;
    }
    vec3 direction = guideSpecularRay.values[2u * index + 1u].xyz;

    // The same world `extend` traces: opaque geometry, curves answered by the
    // same intersector under the same interval and committed-hit rules, and the
    // analytic lights competing on distance.
    // A reflection is a secondary ray, and the guide has to describe the same
    // world the image does: a wall culled for primary rays only is still in a
    // mirror (README semantics rule 3).
    float tGeometry = 1.0e30;
    uint volumes = hdclaude_cull_volumes(false);
    if (volumes == 0u)
    {
        specular_trace(origin.xyz, direction, 1.0e-4, 1.0e30, 0xFFu, false, 0u,
                       tGeometry);
    }
    else
    {
        hdclaude_cull_prepare(origin.xyz, direction, volumes);
        float t = 1.0e-4;
        float stretchEnd;
        for (uint guard = 0u; tGeometry > 1.0e29 &&
                              guard <= HDCLAUDE_MAX_CULL_VOLUMES &&
                              hdclaude_cull_next(t, 1.0e30, volumes, stretchEnd);
             ++guard)
        {
            specular_trace(origin.xyz, direction, t, stretchEnd,
                           HDCLAUDE_MASK_SECONDARY, false, volumes, tGeometry);
            t = stretchEnd;
        }
        if ((frame.cullExceptions & 2u) != 0u)
        {
            specular_trace(origin.xyz, direction, 1.0e-4, tGeometry,
                           HDCLAUDE_MASK_SECONDARY_EXCEPTION, true, volumes,
                           tGeometry);
        }
    }

    float tLight;
    vec3 lightNormal;
    int light = hdclaude_nearest_light(origin.xyz, direction, tGeometry, tLight,
                                       lightNormal);
    float distance = light >= 0 ? tLight : tGeometry;

    // The direction is unit length, so the ray parameter is the world-space
    // distance the guide asks for.
    guideSpecularRay.values[2u * index + 1u].w =
        distance < 1.0e29 ? min(distance, kNoHit) : kNoHit;
}
