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

    vec4 mediumExtinction = pathMedium.values[2u * path + 0u];
    vec4 mediumColour = pathMedium.values[2u * path + 1u];
    bool inMedium = mediumColour.w > 0.5;

    vec4 throughput = pathThroughput.values[path];

    bool hitGeometry = false;
    float tGeometry = 1.0e30;
    int light = -1;
    float tLight = 1.0e30;
    vec3 lightNormal = vec3(0.0, 1.0, 0.0);
    vec2 barycentrics = vec2(0.0);
    int instance = -1;
    int primitive = -1;

    // --- The medium, and the walk's spectral bookkeeping ---------------------
    //
    // Both coefficients are per lane. The medium does not change inside one
    // invocation, so they are resolved once rather than per step.
    //
    // Absorption stays analytic. Sampling a collision and weighting by the
    // single-scattering albedo is the textbook form and is unbiased, but in a
    // medium that only absorbs it turns a closed-form attenuation into a coin
    // flip that kills the path -- right in the mean and far noisier for nothing.
    // Honey and coloured glass are exactly that medium. So the distance is
    // proposed from the *scattering* coefficient alone.
    //
    // The medium arrives as an extinction and a single-scattering albedo, not
    // as the two coefficients the closure authored, and the split happens on
    // the way in rather than here. The reason is that the two are resolved to
    // wavelengths by *different* fits and their ratio does not survive being
    // taken twice: a subsurface material whose colour is entirely a per-channel
    // albedo lost a tenth of its red that way, while the two channels either
    // side of it moved the other way. Whatever else it is, that is not the
    // colour the material was authored with.
    //
    // Split as they are here, each half is resolved in the domain its fit was
    // built for. The albedo is a reflectance -- bounded in [0, 1], which is the
    // sigmoid's own range -- and is upsampled as one, so the per-lane albedo is
    // exactly the authored albedo's spectrum. The extinction is a coefficient
    // and is unbounded, so it goes through its transmittance as before. The
    // two limits are untouched by the change: a medium that only absorbs has an
    // albedo of exactly zero and a lossless one exactly one, and the fit
    // reproduces both exactly.
    vec3 authoredColour = clamp(mediumColour.xyz, vec3(0.0), vec3(1.0));
    bool hasInterior = mediumColour.w > 1.5;
    bool reflectance = mediumColour.w > 2.5;
    bool scatters = hasInterior && max(max(authoredColour.x, authoredColour.y),
                                       authoredColour.z) > 0.0;

    vec4 sigmaT = vec4(0.0);
    vec4 sigmaA = vec4(0.0);
    vec4 sigmaS = vec4(0.0);
    if (hasInterior)
    {
        sigmaT = hdclaude_lane_extinction(mediumExtinction.xyz, lambda);
        sigmaA = sigmaT;
    }
    if (scatters)
    {
        // The colour becomes a spectrum first, and the nonlinear relation is
        // applied to that spectrum rather than to the three numbers it came
        // from. Both orders are defensible-looking and only one of them returns
        // the authored colour, because van de Hulst's relation is steep where
        // subsurface materials live: inverting the RGB first left a magenta
        // material a seventh short in red while the channels either side of it
        // moved the other way, which is what a nonlinearity applied before a
        // projection always does.
        //
        // Clamped because the sigmoid's range is open at one and an albedo a
        // hair over it would make a lossless medium gain light at every
        // collision.
        vec4 colour = clamp(hdclaude_upsample(authoredColour, lambda),
                            vec4(0.0), vec4(1.0));
        vec4 albedo = reflectance
                          ? hdclaude_subsurface_albedo(colour,
                                                       mediumExtinction.w)
                          : colour;
        sigmaS = albedo * sigmaT;
        sigmaA = sigmaT - sigmaS;
    }

    // The lane that proposes every free flight in this walk, chosen uniformly
    // and *once*.
    //
    // Once, not per step, and that is the whole design. Weighting each step by
    // the balance heuristic over the four lanes' step densities is unbiased and
    // bounds each step's weight by the lane count -- but a walk multiplies
    // steps, so the path's weight is bounded only by the lane count raised to
    // the step count, and a dense chromatic medium then refuses to converge.
    // That was measured: per-step selection reads 1.02, 1.06, 1.01 on a
    // four-to-one medium and does not tighten at eight times the samples, while
    // the same estimator on a medium thin enough to scatter once is exact
    // (docs/implementation-notes.md, 2026-09-08).
    //
    // With one proposer for the whole walk the balance heuristic applies to the
    // whole walk's density, and the weight is again one density over the mean of
    // four -- bounded by the lane count over the *path*, which is what the
    // per-step form could not deliver.
    //
    // A fixed control wavelength would have the same shape and be wrong: the
    // weights are only unbiased because this is a random choice among the
    // techniques they average over.
    //
    // Drawn only when there is something to scatter off. Every kernel shares
    // one random stream per path, so a draw taken where it is not needed shifts
    // what every later sampler in the frame sees -- and a surface that merely
    // transmits marks the path as being in a medium whether or not that medium
    // scatters. Guarding on the coefficient rather than on the flag keeps a
    // clear glass, a purely absorbing one, and a path in vacuum on exactly the
    // sequence they had.
    //
    // An achromatic medium needs no draw at all, and that is exact rather than
    // an optimisation. All four lanes then carry the same coefficient, so the
    // four techniques are the same technique, the balance heuristic's average
    // equals any one of them, and every weight is one whichever lane is named.
    // Choosing lane zero is therefore not a fixed control wavelength -- the
    // thing this design exists to avoid -- it is a choice among identical
    // options. It is gated on exact equality of the authored coefficient,
    // because that is the condition under which the claim holds.
    //
    // It also keeps every scene whose medium does not scatter chromatically
    // rendering the sequence it already rendered, which is what makes a change
    // to this kernel attributable to the media it actually affects.
    bool achromatic = authoredColour.x == authoredColour.y &&
                      authoredColour.y == authoredColour.z &&
                      mediumExtinction.x == mediumExtinction.y &&
                      mediumExtinction.y == mediumExtinction.z;
    float control = 0.0;
    if (scatters)
    {
        uint proposer = 0u;
        if (!achromatic)
        {
            proposer = min(uint(hdclaude_random(rng) *
                                float(HDCLAUDE_SPECTRAL_LANES)),
                           uint(HDCLAUDE_SPECTRAL_LANES - 1));
        }
        control = sigmaS[proposer];
    }

    // The walk's density needs only these two. For lane j the un-normalised
    // probability of this exact sequence is
    //
    //     q_j = sigma_s[j]^collisions * exp(-sigma_s[j] * distance)
    //
    // because the per-step factors collapse: a product of exponentials is an
    // exponential of a sum, and every collision contributes one factor of the
    // coefficient. No per-step product has to be carried at all.
    int collisions = 0;
    float distance = 0.0;
    // Compensation for surviving roulette, applied with the weight at the end.
    // Roulette can no longer divide the throughput as it goes, because the
    // throughput is not updated until the walk finishes.
    float rouletteWeight = 1.0;

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

        // A lane with no scattering proposes no collision, which is the honest
        // answer rather than a division by zero; the weights below then hand
        // the sample to whichever lanes do scatter.
        float flight = control > 0.0
                           ? -log(max(1.0e-8, hdclaude_random(rng))) / control
                           : 1.0e30;

        if (flight >= tBoundary || step == kMaxWalkSteps)
        {
            if (tBoundary > 1.0e29)
            {
                // Inside a medium with no boundary anywhere ahead, which can
                // only mean the medium was never closed -- an open shell, or
                // faces with nothing joining their edges. The physical answer
                // over an unbounded scattering medium is that the path never
                // gets out, and reaching this says the scene is wrong rather
                // than the transport.
                throughput = vec4(0.0);
                hitGeometry = false;
                light = -1;
                break;
            }
            distance += tBoundary;

            // The walk is over, so its density is known and the balance
            // heuristic can be applied to the whole of it. Each lane
            // contributes the probability it would have produced this exact
            // sequence, divided by the average over the four lanes that could
            // have proposed it.
            //
            // In logarithms, because `sigma_s^collisions` overflows and
            // `exp(-sigma_s * distance)` underflows long before their ratio
            // does anything interesting. The largest is factored out and
            // cancels exactly, which leaves at least one lane at one and the
            // mean strictly positive however far apart the coefficients are.
            vec4 logDensity = float(collisions) * log(max(sigmaS, vec4(1.0e-30)))
                              - sigmaS * distance;
            float peak = max(max(logDensity.x, logDensity.y),
                             max(logDensity.z, logDensity.w));
            vec4 density = exp(logDensity - peak);
            float mean = 0.25 * (density.x + density.y + density.z + density.w);

            // Every weight is now one density over the mean of four, so no lane
            // can carry more than the lane count however long the walk was.
            // With achromatic coefficients all four are equal, the mean is that
            // value, and every weight is exactly one -- the achromatic walk is
            // this walk's own special case rather than a second path to keep in
            // agreement with it.
            throughput *= (density / mean) * exp(-sigmaA * distance) *
                          rouletteWeight;
            break;
        }

        // A real scattering event. The phase function cancels, being what the
        // direction is sampled from; the coefficients are accounted for once,
        // at the end, by the density above.
        distance += flight;
        ++collisions;

        origin = origin + direction * flight;
        direction = hdclaude_sample_phase(
            direction, mediumExtinction.w,
            vec2(hdclaude_random(rng), hdclaude_random(rng)));

        // Roulette on the walk itself. A dense, dark medium would otherwise
        // spend the whole cap carrying almost nothing.
        //
        // It weighs the absorption accumulated so far rather than the
        // throughput, which is no longer updated as the walk runs. That is the
        // part of the weight which actually decays: in a medium that absorbs
        // nothing this stays at the incoming throughput and roulette never
        // fires, which is right, because such a walk has lost nothing and ends
        // when it reaches a boundary rather than when it gives up.
        vec4 carried = throughput * exp(-sigmaA * distance) * rouletteWeight;
        float survival = clamp(max(max(carried.x, carried.y),
                                   max(carried.z, carried.w)),
                               0.05, 1.0);
        if (hdclaude_random(rng) > survival)
        {
            throughput = vec4(0.0);
            hitGeometry = false;
            light = -1;
            break;
        }
        rouletteWeight /= survival;
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

    // The hit, folded into the call's hash. Keyed on the path as well as the
    // geometry, so two rays swapping which triangle they found is a change and
    // not a cancellation.
    if (hitGeometry)
    {
        // Through a local, because `hdclaude_pcg` advances the state it is
        // given and so takes it `inout`.
        uint hitSeed = uint(record.x) * 2654435761u ^
                       uint(record.y) * 2246822519u ^
                       path * 3266489917u;
        atomicAdd(counters.hitHash, hdclaude_pcg(hitSeed));
    }
}
