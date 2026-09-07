#version 460
#extension GL_EXT_ray_query : require
#include "path_state.glsl"

// Closest-hit traversal for the active queue, and the volumetric walk that
// happens on the way.
//
// Writes a hit record and nothing about *shading*. Keeping shading out of this
// kernel is what lets the next one be dispatched per material, which is the
// whole reason the integrator is wavefront (docs/architecture.md 2).
//
// The medium walk lives here rather than in a kernel of its own because a
// scattering event is not a shading event: it has no material, so the
// per-material sort has no bin for it, and a path that scatters needs another
// traversal rather than another shade. Doing the whole walk inside one
// invocation keeps the wavefront's shape -- a path still leaves this kernel
// with exactly one surface hit -- at the cost of a loop bounded by its own
// roulette rather than by the bounce count.
//
// What that costs: no next-event estimation at a scattering vertex. Light
// reaches the medium by entering it and is found when the walk leaves, so the
// estimate is unbiased and noisier than one that also sampled lights from
// inside. That is the same trade the integrator already makes for a delta
// closure, and it is recorded rather than hidden.

layout(local_size_x = 64) in;

// Long enough that roulette, not the cap, ends a walk in any medium worth
// rendering; short enough that a mis-authored one cannot hang the kernel.
const int kMaxWalkSteps = 256;

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
    vec4 lambda = pathWavelengths.values[path];
    uint rng = pathRng.values[path];

    vec4 mediumAbsorption = pathMedium.values[2u * path + 0u];
    vec4 mediumScattering = pathMedium.values[2u * path + 1u];
    bool inMedium = mediumScattering.w > 0.5;

    vec4 throughput = pathThroughput.values[path];

    bool hitGeometry = false;
    float tGeometry = 1.0e30;
    int light = -1;
    float tLight = 1.0e30;
    vec3 lightNormal = vec3(0.0, 1.0, 0.0);
    vec2 barycentrics = vec2(0.0);
    int instance = -1;
    int primitive = -1;

    for (int step = 0; step <= kMaxWalkSteps; ++step)
    {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, sceneTlas, gl_RayFlagsOpaqueEXT, 0xFF,
                              origin, 0.0, direction, 1.0e30);
        while (rayQueryProceedEXT(query)) { }

        hitGeometry = rayQueryGetIntersectionTypeEXT(query, true) ==
                      gl_RayQueryCommittedIntersectionTriangleEXT;
        tGeometry = hitGeometry ? rayQueryGetIntersectionTEXT(query, true)
                                : 1.0e30;
        if (hitGeometry)
        {
            barycentrics = rayQueryGetIntersectionBarycentricsEXT(query, true);
            instance = rayQueryGetIntersectionInstanceCustomIndexEXT(query, true);
            primitive = rayQueryGetIntersectionPrimitiveIndexEXT(query, true);
        }

        // An analytic light is intersected in closed form and competes with the
        // geometry hit on distance, which is what makes it an opaque emitter: a
        // light in front of a wall hides the wall, and a light behind it does
        // not shine through.
        light = hdclaude_nearest_light(origin, direction, tGeometry, tLight,
                                       lightNormal);
        float tBoundary = (light >= 0) ? tLight : tGeometry;

        if (!inMedium)
        {
            break;
        }

        // Absorption is spectral and deterministic; scattering is sampled and
        // achromatic. Both halves of that are deliberate.
        //
        // Applying absorption by sampling a collision and weighting by the
        // single-scattering albedo is the textbook form and is unbiased, but in
        // a medium that only absorbs it turns a closed-form attenuation into a
        // coin flip that kills the path -- right in the mean and far noisier for
        // nothing. Honey and coloured glass are exactly that medium.
        //
        // Scattering is achromatic because a *chromatic* scattering coefficient
        // sampled against one control wavelength makes every other lane carry
        // `exp((control - sigma_lane) * flight)`, which grows with the flight
        // and compounds over a walk until it overflows. It did: a strongly
        // forward-scattering slab produced NaN before this. Fixing that
        // properly means multiple importance sampling across the lanes'
        // densities, which is a real piece of work and is recorded as remaining.
        // Taking the mean makes every lane share one density, so the scattering
        // weight is exactly one and only absorption carries colour -- which is
        // where a medium's colour comes from in almost every real material.
        vec4 sigmaA = hdclaude_lane_extinction(mediumAbsorption.xyz, lambda);
        float sigmaS = max((mediumScattering.x + mediumScattering.y +
                            mediumScattering.z) / 3.0,
                           0.0);

        float control = max(sigmaS, 1.0e-6);
        float flight = -log(max(1.0e-8, hdclaude_random(rng))) / control;

        if (flight >= tBoundary || step == kMaxWalkSteps)
        {
            // Reached the boundary. The scattering term cancels against its own
            // density exactly, so what is left is the absorption over the
            // flight -- and with no scattering at all this is the whole medium,
            // applied in closed form with no randomness.
            throughput *= exp(-sigmaA * tBoundary);
            break;
        }

        // A real scattering event. The phase function cancels, being what the
        // direction is sampled from, and the scattering coefficient cancels
        // against the density that produced this distance.
        throughput *= exp(-sigmaA * flight);

        origin = origin + direction * flight;
        direction = hdclaude_sample_phase(
            direction, mediumAbsorption.w,
            vec2(hdclaude_random(rng), hdclaude_random(rng)));

        // Roulette on the walk itself. A dense, dark medium would otherwise
        // spend the whole cap carrying almost nothing.
        float survival = clamp(max(max(throughput.x, throughput.y),
                                   max(throughput.z, throughput.w)),
                               0.05, 1.0);
        if (hdclaude_random(rng) > survival)
        {
            throughput = vec4(0.0);
            hitGeometry = false;
            light = -1;
            break;
        }
        throughput /= survival;
    }

    pathThroughput.values[path] = throughput;
    pathRng.values[path] = rng;
    pathOrigin.values[path] = origin;
    pathDirection.values[path] = direction;

    ivec4 record = ivec4(-1, -1, 0, 0);
    if (light >= 0)
    {
        // Encoded below -1 so the sort skips it exactly as it skips a miss, and
        // the kernel that retires missed paths adds the emission. The origin is
        // left at the walk's last vertex, because that kernel re-intersects the
        // light to recover the distance and normal the density needs.
        record.x = -2 - light;
    }
    else if (hitGeometry)
    {
        record.x = instance;
        record.y = primitive;
        record.z = floatBitsToInt(barycentrics.x);
        record.w = floatBitsToInt(barycentrics.y);

        // The hit position replaces the origin, so shading needs no ray
        // parameter and no second evaluation of origin + t * direction.
        pathOrigin.values[path] = origin + direction * tGeometry;
    }
    hits.values[path] = record;
}
