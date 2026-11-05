// Fog volumes: participating media bounded by a mesh, transported during
// traversal.
//
// A placement bound to a material whose only terminal is a MaterialX `volume`
// is not a surface. It is the boundary of a medium -- invisible, index-matched,
// and in no ray's mask but the fog's (HDCLAUDE_MASK_FOG) -- and the medium
// inside it is described *per point* by patterns the host flattened out of the
// VDF and EDF trees (AuthoredVolume). Those patterns are generated into the
// traversal library beside the cut-out opacities, so the kernels that walk a
// ray can ask any volume what it is like anywhere: `hdclaude_material_medium`.
//
// Three things use that:
//
//   `extend` walks a path's ray through every fog it crosses, by spectral
//   tracking against each volume's majorant. Emission is gathered on the way;
//   a real collision either absorbs the path or makes a scattering *vertex*,
//   which samples a light (next-event estimation) and a new direction there
//   and then, and puts the path on the next bounce's queue.
//
//   `shadow` attenuates a light sample by the fog between the vertex and the
//   light, by ratio tracking.
//
//   `extend`, in its majorant mode, samples every volume densely at the start
//   of a frame to bound its extinction.
//
// Which volumes a ray is inside is decided by *parity*, not by facing: every
// crossing of a volume's boundary along the whole ray is found, and a volume
// crossed an odd number of times contains the ray's origin. A closed mesh
// gives the right answer that way whichever way its faces wind, and a path
// carries no record of which fog it is in -- the record the interior media
// keep cannot nest, and fog volumes overlap and contain surfaces routinely.
//
// Units: every coefficient is per scene unit (AuthoredVolume).
//
// Included after path_state.glsl and cutout.glsl. Without a traversal library
// that has volumes, nothing here is compiled but the stubs at the end, and
// the host publishes no fog volume, so `frame.fogVolumeCount` is zero.

#ifndef HDCLAUDE_FOG_GLSL
#define HDCLAUDE_FOG_GLSL

/// Boundary crossings a ray collects. A ray through one convex volume crosses
/// two; eight leaves room for overlapping volumes and concave boundaries. A ray
/// that crosses more ignores the rest, which can misjudge which volumes contain
/// its origin and is the price of a fixed array.
#define HDCLAUDE_FOG_MAX_CROSSINGS 8

/// Tentative collisions one walk may examine before it gives up and lets the
/// ray through. Generous: a majorant within its margin of the real extinction
/// makes a few per mean free path.
const int kHdclaudeFogMaxSteps = 1024;

/// The majorant is sampled on this many points along each axis of a volume's
/// bounds -- a lattice that reaches every face, and a jittered point in each of
/// its cells -- and kept with this margin above the largest it finds.
const uint kHdclaudeFogMajorantResolution = 32u;
const float kHdclaudeFogMajorantMargin = 1.5;

/// The fog volume a placement bounds, or -1.
int hdclaude_fog_of(int instance)
{
    for (uint i = 0u; i < frame.fogVolumeCount; ++i)
    {
        if (fogVolumes.values[i].instance == uint(instance))
        {
            return int(i);
        }
    }
    return -1;
}

float hdclaude_fog_majorant(uint fog)
{
    return uintBitsToFloat(fogMajorants.values[fog]);
}

/// Henyey-Greenstein, in the propagation form `hdclaude_sample_phase` samples:
/// `cosTheta` is between the direction of travel and the scattered direction,
/// and a positive `g` favours going on.
float hdclaude_henyey_greenstein(float g, float cosTheta)
{
    float d = max(1.0 + g * g - 2.0 * g * cosTheta, 1.0e-8);
    return (1.0 - g * g) / (12.5663706144 * d * sqrt(d));
}

/// The phase function of a medium: its lobes, each weighted by its share of
/// the scattering. Normalised over the sphere, so it is its own density.
float hdclaude_fog_phase(vec4 g, vec4 w, vec3 forward, vec3 scattered)
{
    float total = w.x + w.y + w.z + w.w;
    float cosTheta = dot(forward, scattered);
    if (!(total > 0.0))
    {
        return 1.0 / 12.5663706144;
    }
    return (w.x * hdclaude_henyey_greenstein(g.x, cosTheta) +
            w.y * hdclaude_henyey_greenstein(g.y, cosTheta) +
            w.z * hdclaude_henyey_greenstein(g.z, cosTheta) +
            w.w * hdclaude_henyey_greenstein(g.w, cosTheta)) /
           total;
}

/// A direction from that phase function: a lobe chosen by its weight, then
/// sampled. The mixture's density is `hdclaude_fog_phase`, which equals the
/// mixture's value, so a scattered path keeps its throughput.
vec3 hdclaude_fog_sample_phase(vec4 g, vec4 w, vec3 forward, vec3 u)
{
    float total = w.x + w.y + w.z + w.w;
    float lobe = 0.0;
    if (total > 0.0)
    {
        float pick = u.z * total;
        lobe = pick < w.x ? g.x
             : pick < w.x + w.y ? g.y
             : pick < w.x + w.y + w.z ? g.z
                                      : g.w;
    }
    return hdclaude_sample_phase(forward, lobe, u.xy);
}

/// Every crossing of a fog boundary along the ray from `origin`, nearest
/// first, and which volume each belongs to. Traced to infinity, not to the
/// ray's end, because the crossings beyond the end are what say which volumes
/// the origin is inside.
int hdclaude_fog_crossings(vec3 origin, vec3 direction,
                           out float crossings[HDCLAUDE_FOG_MAX_CROSSINGS],
                           out uint volumes[HDCLAUDE_FOG_MAX_CROSSINGS])
{
    // Stepped past each crossing by an amount that stays meaningful at any
    // scene scale, as hdclaude_offset_ray's is.
    float scale = max(1.0, max(abs(origin.x), max(abs(origin.y), abs(origin.z))));
    int count = 0;
    float t = 0.0;
    for (int k = 0; k < HDCLAUDE_FOG_MAX_CROSSINGS; ++k)
    {
        rayQueryEXT query;
        rayQueryInitializeEXT(query, sceneTlas, gl_RayFlagsOpaqueEXT,
                              HDCLAUDE_MASK_FOG, origin, t, direction, 1.0e30);
        while (rayQueryProceedEXT(query))
        {
        }
        if (rayQueryGetIntersectionTypeEXT(query, true) ==
            gl_RayQueryCommittedIntersectionNoneEXT)
        {
            break;
        }
        float hit = rayQueryGetIntersectionTEXT(query, true);
        int volume =
            hdclaude_fog_of(rayQueryGetIntersectionInstanceCustomIndexEXT(query, true));
        if (volume >= 0)
        {
            crossings[count] = hit;
            volumes[count] = uint(volume);
            ++count;
        }
        t = hit + 1.0e-5 * max(scale, hit);
    }
    return count;
}

#ifdef HDCLAUDE_TRAVERSAL_VOLUME

/// One volume's medium at a world-space point, as the material authored it.
HdclaudeMedium hdclaude_fog_medium(uint fog, vec3 position)
{
    FogVolume volume = fogVolumes.values[fog];
    InstanceGeometry geometry = instances.values[volume.instance];
    vec3 objectPosition = vec4(position, 1.0) * geometry.worldToObject;
    // A point in a medium has a position and nothing else; the frame is any
    // orthonormal one, so a graph that reads a normal gets a unit vector
    // rather than an undefined one.
    hdclaude_set_surface_hit(position, vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0),
                             vec3(0.0, 1.0, 0.0), objectPosition,
                             vec3(0.0, 0.0, 1.0), vec3(1.0, 0.0, 0.0),
                             vec3(0.0, 1.0, 0.0), vec2(0.0),
                             geometry.displayColor, vec3(0.0, 0.0, -1.0));
    HdclaudeMedium medium = hdclaude_material_medium(volume.material);
    // A coefficient below zero is not a medium. Clamped where it is used as
    // one, as a cut-out opacity is clamped where it is used as a probability.
    medium.absorption = max(medium.absorption, vec3(0.0));
    medium.scattering = max(medium.scattering, vec3(0.0));
    medium.emission = max(medium.emission, vec3(0.0));
    medium.weight = max(medium.weight, vec4(0.0));
    medium.anisotropy = clamp(medium.anisotropy, vec4(-0.99), vec4(0.99));
    return medium;
}

/// Every volume in `inside` at one point, summed: coefficients and emission
/// add, and the lobes are pooled -- each keeps its weight, which is already
/// its share of the scattering in absolute terms -- up to four.
HdclaudeMedium hdclaude_fog_media(uint inside, vec3 position)
{
    HdclaudeMedium sum = HdclaudeMedium(vec3(0.0), vec3(0.0), vec3(0.0),
                                        vec4(0.0), vec4(0.0));
    int lobes = 0;
    uint remaining = inside;
    while (remaining != 0u)
    {
        uint fog = uint(findLSB(remaining));
        remaining &= remaining - 1u;
        HdclaudeMedium medium = hdclaude_fog_medium(fog, position);
        sum.absorption += medium.absorption;
        sum.scattering += medium.scattering;
        sum.emission += medium.emission;
        for (int k = 0; k < 4; ++k)
        {
            if (medium.weight[k] > 0.0 && lobes < 4)
            {
                sum.anisotropy[lobes] = medium.anisotropy[k];
                sum.weight[lobes] = medium.weight[k];
                ++lobes;
            }
        }
    }
    return sum;
}

/// A medium's extinction on the four lanes, and the share of it that scatters.
///
/// Resolved the way an interior medium is (extend.comp.glsl): the extinction
/// through its transmittance, the albedo as the reflectance it is. An
/// achromatic medium -- which nearly every fog is -- skips both fits and is
/// exact, because a grey's spectrum is the constant the fit only approximates.
void hdclaude_fog_lanes(HdclaudeMedium medium, vec4 lambda, out vec4 sigmaA,
                        out vec4 sigmaS)
{
    vec3 extinction = medium.absorption + medium.scattering;
    vec3 albedo = vec3(extinction.x > 0.0 ? medium.scattering.x / extinction.x : 0.0,
                       extinction.y > 0.0 ? medium.scattering.y / extinction.y : 0.0,
                       extinction.z > 0.0 ? medium.scattering.z / extinction.z : 0.0);
    vec4 sigmaT = extinction.x == extinction.y && extinction.y == extinction.z
                      ? vec4(extinction.x)
                      : hdclaude_lane_extinction(extinction, lambda);
    vec4 lanes = albedo.x == albedo.y && albedo.y == albedo.z
                     ? vec4(albedo.x)
                     : clamp(hdclaude_upsample(albedo, lambda), vec4(0.0),
                             vec4(1.0));
    sigmaS = lanes * sigmaT;
    sigmaA = sigmaT - sigmaS;
}

/// The sum of the majorants of the volumes in `inside`.
float hdclaude_fog_bound(uint inside)
{
    float bound = 0.0;
    uint remaining = inside;
    while (remaining != 0u)
    {
        uint fog = uint(findLSB(remaining));
        remaining &= remaining - 1u;
        bound += hdclaude_fog_majorant(fog);
    }
    return bound;
}

/// The walk of a path's ray through the fog over `[0, tEnd]`.
///
/// Spectral tracking (Kutz et al. 2017): one free flight for all four lanes,
/// against a majorant that bounds every lane, and each tentative collision is
/// an absorption, a scattering or a null collision with probabilities from the
/// lanes' *average* coefficients. The throughput takes each lane's own share of
/// the event over that average, so the estimate is unbiased on every lane and
/// an achromatic medium -- where every share is one -- is plain delta tracking.
///
/// Emission is gathered at every tentative collision as `j / majorant`, which
/// integrates `T(t) j(t)` along the ray in expectation: points arrive at the
/// majorant's rate for as long as the walk survives, and it survives to `t`
/// with probability `T(t)`. MaterialX's EDF in a volume is radiance *per unit
/// length*, which is what `j` is.
///
/// Returns 0 when the ray gets through, 1 when it scatters at `tScatter` --
/// with that point's phase lobes and the fog to name the vertex by -- and 2
/// when it is absorbed, with the throughput zeroed.
int hdclaude_fog_walk(vec3 origin, vec3 direction, float tEnd, vec4 lambda,
                      inout uint rng, inout vec4 throughput, inout vec4 radiance,
                      out float tScatter, out vec4 lobeAnisotropy,
                      out vec4 lobeWeight, out int fogInstance)
{
    tScatter = tEnd;
    lobeAnisotropy = vec4(0.0);
    lobeWeight = vec4(0.0);
    fogInstance = -1;

    float crossings[HDCLAUDE_FOG_MAX_CROSSINGS];
    uint volumes[HDCLAUDE_FOG_MAX_CROSSINGS];
    int count = hdclaude_fog_crossings(origin, direction, crossings, volumes);
    if (count == 0)
    {
        return 0;
    }
    // The volumes crossed an odd number of times contain the origin.
    uint inside = 0u;
    for (int i = 0; i < count; ++i)
    {
        inside ^= 1u << volumes[i];
    }

    float begin = 0.0;
    int steps = 0;
    for (int i = 0; i <= count; ++i)
    {
        float end = min(i < count ? crossings[i] : tEnd, tEnd);
        // Inside a volume with no crossing left ahead and nothing to stop
        // at: an open boundary, or one the crossing cap cut short. The fog
        // ends at the last crossing found rather than at infinity.
        if (end > 1.0e29)
        {
            break;
        }
        float bound = inside != 0u ? hdclaude_fog_bound(inside) : 0.0;
        if (bound > 0.0 && end > begin)
        {
            float t = begin;
            while (steps < kHdclaudeFogMaxSteps)
            {
                ++steps;
                t -= log(max(1.0e-8, 1.0 - hdclaude_random(rng))) / bound;
                if (t >= end)
                {
                    break;
                }
                vec3 position = origin + direction * t;
                HdclaudeMedium medium = hdclaude_fog_media(inside, position);
                vec4 sigmaA;
                vec4 sigmaS;
                hdclaude_fog_lanes(medium, lambda, sigmaA, sigmaS);
                // Negative only where the majorant failed to bound; such a
                // point is treated as having no null collisions rather than a
                // negative probability of one.
                vec4 sigmaN = max(vec4(bound) - sigmaA - sigmaS, vec4(0.0));

                if (max(max(medium.emission.x, medium.emission.y),
                        medium.emission.z) > 0.0)
                {
                    radiance += throughput *
                                hdclaude_upsample_emission(medium.emission, lambda) /
                                bound;
                }

                float pA = 0.25 * (sigmaA.x + sigmaA.y + sigmaA.z + sigmaA.w);
                float pS = 0.25 * (sigmaS.x + sigmaS.y + sigmaS.z + sigmaS.w);
                float pN = 0.25 * (sigmaN.x + sigmaN.y + sigmaN.z + sigmaN.w);
                float total = pA + pS + pN;
                if (!(total > 0.0))
                {
                    continue;
                }
                float u = hdclaude_random(rng) * total;
                if (u < pA)
                {
                    throughput = vec4(0.0);
                    return 2;
                }
                if (u < pA + pS)
                {
                    throughput *= sigmaS * (total / (bound * pS));
                    tScatter = t;
                    lobeAnisotropy = medium.anisotropy;
                    lobeWeight = medium.weight;
                    fogInstance =
                        int(fogVolumes.values[uint(findLSB(inside))].instance);
                    return 1;
                }
                throughput *= sigmaN * (total / (bound * pN));
            }
        }
        if (i < count)
        {
            if (crossings[i] >= tEnd)
            {
                break;
            }
            inside ^= 1u << volumes[i];
            begin = crossings[i];
        }
    }
    return 0;
}

/// The fog's transmittance along a shadow ray over `[tMin, tMax]`, by ratio
/// tracking: the same free flights, and at each the lanes' chance of *not*
/// colliding, multiplied up. Unbiased, and never a hard zero where the fog is
/// thin, which is what keeps a light seen through haze from flickering.
vec4 hdclaude_fog_transmittance(vec3 origin, vec3 direction, float tMin,
                                float tMax, vec4 lambda, inout uint rng)
{
    float crossings[HDCLAUDE_FOG_MAX_CROSSINGS];
    uint volumes[HDCLAUDE_FOG_MAX_CROSSINGS];
    int count = hdclaude_fog_crossings(origin, direction, crossings, volumes);
    if (count == 0)
    {
        return vec4(1.0);
    }
    uint inside = 0u;
    for (int i = 0; i < count; ++i)
    {
        inside ^= 1u << volumes[i];
    }

    vec4 transmittance = vec4(1.0);
    float begin = 0.0;
    int steps = 0;
    for (int i = 0; i <= count; ++i)
    {
        float end = min(i < count ? crossings[i] : tMax, tMax);
        if (end > 1.0e29)
        {
            break;
        }
        float bound = inside != 0u ? hdclaude_fog_bound(inside) : 0.0;
        float t = max(begin, tMin);
        if (bound > 0.0 && end > t)
        {
            while (steps < kHdclaudeFogMaxSteps)
            {
                ++steps;
                t -= log(max(1.0e-8, 1.0 - hdclaude_random(rng))) / bound;
                if (t >= end)
                {
                    break;
                }
                HdclaudeMedium medium =
                    hdclaude_fog_media(inside, origin + direction * t);
                vec4 sigmaA;
                vec4 sigmaS;
                hdclaude_fog_lanes(medium, lambda, sigmaA, sigmaS);
                transmittance *=
                    max(vec4(1.0) - (sigmaA + sigmaS) / bound, vec4(0.0));

                // Roulette once little is left, so a light deep in dense fog
                // does not walk the whole of it to deliver nothing.
                float largest = max(max(transmittance.x, transmittance.y),
                                    max(transmittance.z, transmittance.w));
                if (largest < 0.1)
                {
                    if (!(largest > 0.0) || hdclaude_random(rng) > largest)
                    {
                        return vec4(0.0);
                    }
                    transmittance /= largest;
                }
            }
        }
        if (i < count)
        {
            if (crossings[i] >= tMax)
            {
                break;
            }
            inside ^= 1u << volumes[i];
            begin = crossings[i];
        }
    }
    return transmittance;
}

/// One invocation of the majorant pass: two points of one volume's bounds --
/// a lattice point, so every face is reached, and a jittered point in the cell
/// it opens -- and the larger extinction found, with the margin, folded into
/// the volume's majorant.
///
/// A sampled bound rather than a proven one, because the medium is an
/// arbitrary MaterialX graph and only evaluating it says what it is. A smooth
/// field -- every fog density in practice -- is bounded with room to spare at
/// this density; a graph with features finer than a lattice cell can exceed it
/// there, and the walk then treats such a point as having no null collisions,
/// which under-attenuates it rather than failing.
void hdclaude_fog_build_majorant(uint invocation)
{
    const uint resolution = kHdclaudeFogMajorantResolution;
    const uint perVolume = resolution * resolution * resolution;
    uint fog = invocation / perVolume;
    if (fog >= frame.fogVolumeCount)
    {
        return;
    }
    uint index = invocation % perVolume;
    uvec3 cell = uvec3(index % resolution, (index / resolution) % resolution,
                       index / (resolution * resolution));

    FogVolume volume = fogVolumes.values[fog];
    InstanceGeometry geometry = instances.values[volume.instance];

    uint seed = invocation * 2654435761u + 0x9e3779b9u;
    vec3 jitter = vec3(hdclaude_random(seed), hdclaude_random(seed),
                       hdclaude_random(seed));

    float bound = 0.0;
    for (int k = 0; k < 2; ++k)
    {
        vec3 fraction = k == 0 ? vec3(cell) / float(resolution - 1u)
                               : (vec3(cell) + jitter) / float(resolution);
        vec3 objectPoint = mix(volume.lo, volume.hi, fraction);
        vec3 position = vec4(objectPoint, 1.0) * geometry.objectToWorld;
        HdclaudeMedium medium = hdclaude_fog_medium(fog, position);
        vec3 extinction = medium.absorption + medium.scattering;
        bound = max(bound, kHdclaudeFogMajorantMargin *
                               max(extinction.x, max(extinction.y, extinction.z)));
        // A medium that emits needs points to gather emission at even where it
        // has nothing to collide with.
        if (max(medium.emission.x, max(medium.emission.y, medium.emission.z)) > 0.0)
        {
            bound = max(bound, volume.emissionRate);
        }
    }
    if (bound > 0.0 && !isinf(bound) && !isnan(bound))
    {
        atomicMax(fogMajorants.values[fog], floatBitsToUint(bound));
    }
}

#else  // HDCLAUDE_TRAVERSAL_VOLUME

// No library, so no volume: the host publishes none and these are unreached.
int hdclaude_fog_walk(vec3 origin, vec3 direction, float tEnd, vec4 lambda,
                      inout uint rng, inout vec4 throughput, inout vec4 radiance,
                      out float tScatter, out vec4 lobeAnisotropy,
                      out vec4 lobeWeight, out int fogInstance)
{
    tScatter = tEnd;
    lobeAnisotropy = vec4(0.0);
    lobeWeight = vec4(0.0);
    fogInstance = -1;
    return 0;
}

vec4 hdclaude_fog_transmittance(vec3 origin, vec3 direction, float tMin,
                                float tMax, vec4 lambda, inout uint rng)
{
    return vec4(1.0);
}

void hdclaude_fog_build_majorant(uint invocation)
{
}

#endif  // HDCLAUDE_TRAVERSAL_VOLUME

/// Next-event estimation from a scattering vertex in fog.
///
/// The surface version's emitter choice, light linking and multiple importance
/// sampling, with the phase function where a closure's response and density
/// were (shade.comp.glsl): one emitter chosen uniformly, its sample weighed by
/// the balance heuristic against the phase function's density wherever a
/// scattered ray could also find it, and the contribution handed to the shadow
/// kernel -- which attenuates it by the fog on the way, and by everything else
/// that occludes. The path is not shaded this bounce, so this is its one shadow
/// ray, as the shadow kernel's lock-free accumulation requires.
///
/// `fogInstance` stands in for the surface a light links: a fog is linked to a
/// light as its boundary placement is.
void hdclaude_fog_next_event(uint path, vec3 position, vec3 forward, vec4 lambda,
                             vec4 throughput, vec4 lobeAnisotropy,
                             vec4 lobeWeight, int fogInstance, inout uint rng)
{
    uint emitters = hdclaude_emitter_count();
    uint emitter = min(uint(hdclaude_random(rng) * float(emitters)), emitters - 1u);
    float selectionPdf = 1.0 / float(emitters);
    vec2 lightU = vec2(hdclaude_random(rng), hdclaude_random(rng));

    LightSample lightSample;
    bool environmentSample = emitter == frame.lightCount;
    bool sunSample = emitter > frame.lightCount;
    int lightLink = -1;
    int shadowLink = -1;
    if (sunSample)
    {
        lightSample.direction = normalize(frame.sunDirection.xyz);
        lightSample.distance = 1.0e30;
        lightSample.radiance = frame.sunRadiance.rgb;
        lightSample.pdf = 1.0;
        lightSample.castsShadows = true;
        lightSample.colorTemperature = 0.0;
        lightSample.temperatureScale = 1.0;
    }
    else if (environmentSample)
    {
        EnvironmentSample environmentDirection = hdclaude_sample_environment(lightU);
        lightSample.direction = environmentDirection.direction;
        lightSample.distance = 1.0e30;
        lightSample.radiance = hdclaude_environment(lightSample.direction);
        lightSample.pdf = environmentDirection.pdf;
        lightSample.castsShadows = true;
        lightSample.colorTemperature = frame.environmentTemperature;
        lightSample.temperatureScale = frame.environmentTemperatureScale;
        lightLink = frame.domeLightLink;
        shadowLink = frame.domeShadowLink;
    }
    else
    {
        lightSample = hdclaude_sample_light(emitter, position, lightU);
        lightLink = lights.values[emitter].lightLink;
        shadowLink = lights.values[emitter].shadowLink;
    }
    if (!hdclaude_linked(fogInstance, lightLink) || !(lightSample.pdf > 0.0))
    {
        return;
    }

    float phase = hdclaude_fog_phase(lobeAnisotropy, lobeWeight, forward,
                                     lightSample.direction);

    // Hittable exactly when the surface version says so: a scattered ray can
    // reach the emitter, so the two strategies share it.
    bool hittable = !sunSample && shadowLink < 0;
    if (hittable && emitter < frame.lightCount)
    {
        hittable = hdclaude_light_geometry_visible(lights.values[emitter]);
    }
    float weight = hittable ? hdclaude_mis_weight(lightSample.pdf * selectionPdf, phase)
                            : 1.0;

    vec4 contribution =
        throughput * phase *
        hdclaude_upsample_emission(lightSample.radiance, lambda,
                                   lightSample.colorTemperature,
                                   lightSample.temperatureScale) *
        weight / (lightSample.pdf * selectionPdf);
    if (!(dot(contribution, contribution) > 0.0))
    {
        return;
    }
    if (!lightSample.castsShadows)
    {
        pathRadiance.values[path] += contribution;
        return;
    }
    uint index = atomicAdd(counters.shadowCount, 1u);
    if (index < frame.pathCount)
    {
        ShadowRay ray;
        // No offset: a point in a medium is on no surface to escape.
        ray.origin = position;
        ray.direction = lightSample.direction;
        ray.contribution = contribution;
        ray.maxDistance = lightSample.distance * 0.9999;
        ray.path = path;
        ray.shadowLink = shadowLink;
        ray.pad1 = 0u;
        shadowRays.values[index] = ray;
    }
}

#endif  // HDCLAUDE_FOG_GLSL
