// The shade kernel body.
//
// This text is appended to a *generated MaterialX material*, so it sees that
// material's ABI: `hdclaude_material_shade`, `hdclaude_set_surface_hit`, and the
// result globals. One compiled pipeline per material, dispatched over the paths
// that hit it -- which is what makes an arbitrarily large MaterialX program cost
// occupancy only on the surfaces that use it (docs/architecture.md 2).
//
// The material module already emits `#version` and its extension directives, so
// this file must not: GLSL requires them before any code, and there is code
// above.

#include "path_state.glsl"

layout(local_size_x = 64) in;

layout(push_constant) uniform ShadeParams {
    /// Which material this pipeline was compiled for. One pipeline exists per
    /// distinct material, and each skips the paths that did not hit its own.
    ///
    /// Until the sort lands, every pipeline is dispatched over the whole active
    /// queue and most invocations exit here. That is already correct and
    /// already gives the register-footprint benefit the design is for -- the
    /// sort removes the wasted lanes, which is a throughput win rather than a
    /// correctness one (docs/wavefront-integrator.md 3).
    uint materialId;
} shadeParams;

/// Interpolated geometry at a hit.
///
/// The normals are the *true* ones, not turned to face the viewer. A closure
/// decides which side of an interface it is on from the sign of dot(N, V), so
/// flipping before the closure sees it makes every refraction look like an
/// entry and glass can never exit itself. Every MaterialX closure calls
/// mx_forward_facing_normal itself, so handing over the true normal costs
/// nothing and is what makes transmission work.
///
/// Geometric decisions -- which way to offset a ray, which side a light is on
/// -- need a viewer-facing normal instead, so it is carried alongside rather
/// than recomputed at each use.
struct SurfacePoint {
    vec3 position;
    vec3 shadingNormal;
    vec3 geometricNormal;
    /// geometricNormal, turned to the side the incoming ray came from.
    vec3 frontGeometricNormal;
    vec3 tangent;
    vec2 uv;
};

/// Reconstruct the hit from the record and the instance's geometry buffers.
///
/// The geometric normal is kept alongside the shading normal: ray offsets and
/// facing decisions must use the true triangle, or a shading normal that has
/// been bent by interpolation or a normal map lets rays start on the wrong side
/// of the surface.
SurfacePoint hdclaude_reconstruct(ivec4 record, vec3 rayDirection, vec3 hitPosition)
{
    SurfacePoint point;
    InstanceGeometry geometry = instances.values[record.x];

    IndexBuffer indices = IndexBuffer(geometry.indices);
    PositionBuffer positions = PositionBuffer(geometry.positions);

    uint i0 = indices.values[record.y * 3 + 0];
    uint i1 = indices.values[record.y * 3 + 1];
    uint i2 = indices.values[record.y * 3 + 2];

    vec3 p0 = positions.values[i0];
    vec3 p1 = positions.values[i1];
    vec3 p2 = positions.values[i2];

    float u = intBitsToFloat(record.z);
    float v = intBitsToFloat(record.w);
    float w = 1.0 - u - v;

    point.position = hitPosition;

    // Object-space geometric normal, then to world through the inverse
    // transpose. Using the transform directly would be wrong under non-uniform
    // scale, which is common once instancing is involved.
    vec3 objectGeometric = normalize(cross(p1 - p0, p2 - p0));
    mat3 normalMatrix = transpose(mat3(geometry.worldToObject[0].xyz,
                                       geometry.worldToObject[1].xyz,
                                       geometry.worldToObject[2].xyz));
    point.geometricNormal = normalize(normalMatrix * objectGeometric);

    if (geometry.normals != 0ul)
    {
        NormalBuffer normals = NormalBuffer(geometry.normals);
        vec3 objectShading = normalize(w * normals.values[i0] +
                                       u * normals.values[i1] +
                                       v * normals.values[i2]);
        point.shadingNormal = normalize(normalMatrix * objectShading);
    }
    else
    {
        point.shadingNormal = point.geometricNormal;
    }

    // The viewer-facing copy, for ray offsets and light-side tests. The true
    // normals above are left alone; see the note on SurfacePoint.
    point.frontGeometricNormal = dot(point.geometricNormal, rayDirection) > 0.0
                                     ? -point.geometricNormal
                                     : point.geometricNormal;

    if (geometry.uvs != 0ul)
    {
        UvBuffer uvs = UvBuffer(geometry.uvs);
        point.uv = w * uvs.values[i0] + u * uvs.values[i1] + v * uvs.values[i2];
    }
    else
    {
        point.uv = vec2(u, v);
    }

    // A tangent orthogonal to the shading normal. Derived from the triangle
    // edge rather than an arbitrary axis, so anisotropic closures rotate with
    // the surface instead of with the world.
    vec3 edge = p1 - p0;
    vec3 worldEdge = mat3(geometry.objectToWorld[0].xyz,
                          geometry.objectToWorld[1].xyz,
                          geometry.objectToWorld[2].xyz) * edge;
    point.tangent = normalize(worldEdge - point.shadingNormal *
                                              dot(point.shadingNormal, worldEdge));
    if (!(dot(point.tangent, point.tangent) > 0.5))
    {
        // Degenerate edge; any orthogonal direction will do.
        vec3 fallback = abs(point.shadingNormal.z) < 0.9 ? vec3(0.0, 0.0, 1.0)
                                                         : vec3(1.0, 0.0, 0.0);
        point.tangent = normalize(cross(fallback, point.shadingNormal));
    }

    return point;
}

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];

    ivec4 record = hits.values[path];
    if (record.x < 0)
    {
        return;   // missed; the environment kernel owns this path
    }
    if (hdclaude_material_of(instances.values[record.x], record.y) !=
        shadeParams.materialId)
    {
        return;   // a different material's dispatch owns this path
    }

    vec3 throughput = pathThroughput.values[path];
    if (dot(throughput, throughput) <= 0.0)
    {
        return;
    }

    vec3 rayDirection = pathDirection.values[path];
    vec3 hitPosition = pathOrigin.values[path];
    SurfacePoint point = hdclaude_reconstruct(record, rayDirection, hitPosition);

    vec3 V = -rayDirection;
    uint rng = pathRng.values[path];

    // Hand the geometry to the generated material. The setter assigns only the
    // members this material actually reads.
    hdclaude_set_surface_hit(point.position, point.shadingNormal, point.tangent);
    hdclaude_wavelengths = vec4(450.0, 550.0, 600.0, 650.0);

    // --- Emission -----------------------------------------------------------
    ClosureData emissionData = ClosureData(CLOSURE_TYPE_EMISSION, vec3(0.0), V,
                                           point.shadingNormal, point.position, 1.0);
    hdclaude_material_shade(emissionData);
    pathRadiance.values[path] += throughput * hdclaude_emission;

    // --- Next-event estimation ----------------------------------------------
    //
    // One light per bounce, chosen uniformly. Evaluating the closure at the
    // light direction yields both the response and the density in a single
    // graph traversal, which is what the closure protocol was shaped to allow
    // (docs/materialx-codegen.md 2).
    //
    // Choosing uniformly rather than by power is deliberate for now: a power
    // heuristic needs a distribution rebuilt whenever a light changes, and
    // getting that stale is a much subtler bug than the extra variance.
    //
    // No MIS. hdClaude's lights are analytic and absent from the acceleration
    // structure, so a scattered ray cannot hit one and there is nothing to
    // double count. Emissive *geometry* is found by the scattered ray and its
    // emission is added on hit, above.
    if (frame.lightCount > 0u)
    {
        uint lightIndex = min(uint(hdclaude_random(rng) * float(frame.lightCount)),
                              frame.lightCount - 1u);
        float selectionPdf = 1.0 / float(frame.lightCount);

        vec2 lightU = vec2(hdclaude_random(rng), hdclaude_random(rng));
        LightSample lightSample =
            hdclaude_sample_light(lightIndex, point.position, lightU);

        if (lightSample.pdf > 0.0 &&
            dot(lightSample.direction, point.frontGeometricNormal) > 0.0)
        {
            hdclaude_sample_u = vec3(hdclaude_random(rng), hdclaude_random(rng),
                                     hdclaude_random(rng));
            ClosureData lightData = ClosureData(CLOSURE_TYPE_REFLECTION,
                                                lightSample.direction, V,
                                                point.shadingNormal,
                                                point.position, 1.0);
            hdclaude_material_shade(lightData);

            // A delta closure has no finite response at any single direction,
            // so next-event estimation contributes nothing to it and the
            // scattered ray is what finds the light.
            if (hdclaude_bsdf.isDelta < 0.5)
            {
                // The estimator, written out: the closure's response already
                // carries the cosine -- every MaterialX reflection response
                // does -- so what remains is the emitted radiance divided by
                // the density of having chosen this direction, which is the
                // light's solid-angle density times the chance of having
                // picked this light.
                vec3 contribution = throughput * hdclaude_bsdf.response *
                                    lightSample.radiance /
                                    (lightSample.pdf * selectionPdf);

                if (dot(contribution, contribution) > 0.0)
                {
                    if (!lightSample.castsShadows)
                    {
                        // Unoccluded by definition: no shadow ray, and the
                        // contribution lands directly.
                        pathRadiance.values[path] += contribution;
                    }
                    else
                    {
                        uint index = atomicAdd(counters.shadowCount, 1u);
                        if (index < frame.pathCount)
                        {
                            ShadowRay ray;
                            ray.origin = hdclaude_offset_ray(
                                point.position, point.frontGeometricNormal);
                            ray.direction = lightSample.direction;
                            ray.contribution = contribution;
                            // Stop just short of the light so the light's own
                            // backing geometry, if the scene has any, does not
                            // occlude it.
                            ray.maxDistance = lightSample.distance * 0.9999;
                            ray.path = path;
                            ray.pad0 = 0u; ray.pad1 = 0u; ray.pad2 = 0u;
                            shadowRays.values[index] = ray;
                        }
                    }
                }
            }
        }
    }
    else
    {
        // No lights in the scene: the stand-in sun.
        //
        // Kept as a fallback rather than deleted, because a stage with no
        // UsdLux prim at all -- a bare mesh dropped into usdview, or a unit
        // test -- would otherwise render as a silhouette against the sky with
        // no way to tell a lighting gap from a shading bug.
        vec3 sunDirection = normalize(frame.sunDirection.xyz);
        if (dot(sunDirection, point.frontGeometricNormal) > 0.0)
        {
            hdclaude_sample_u = vec3(hdclaude_random(rng), hdclaude_random(rng),
                                     hdclaude_random(rng));
            ClosureData lightData = ClosureData(CLOSURE_TYPE_REFLECTION, sunDirection,
                                                V, point.shadingNormal,
                                                point.position, 1.0);
            hdclaude_material_shade(lightData);

            if (hdclaude_bsdf.isDelta < 0.5)
            {
                vec3 contribution = throughput * hdclaude_bsdf.response *
                                    frame.sunRadiance.rgb;
                if (dot(contribution, contribution) > 0.0)
                {
                    uint index = atomicAdd(counters.shadowCount, 1u);
                    if (index < frame.pathCount)
                    {
                        ShadowRay ray;
                        ray.origin = hdclaude_offset_ray(
                            point.position, point.frontGeometricNormal);
                        ray.direction = sunDirection;
                        ray.contribution = contribution;
                        ray.maxDistance = 1.0e30;
                        ray.path = path;
                        ray.pad0 = 0u; ray.pad1 = 0u; ray.pad2 = 0u;
                        shadowRays.values[index] = ray;
                    }
                }
            }
        }
    }

    // --- Scatter -------------------------------------------------------------
    if (frame.bounce + 1u >= frame.maxBounces)
    {
        pathThroughput.values[path] = vec3(0.0);
        pathRng.values[path] = rng;
        return;
    }

    // Pass 1: a direction. Pass 2: the response and density at it. Two passes
    // because a combinator can only mix densities of a common direction.
    hdclaude_sample_u = vec3(hdclaude_random(rng), hdclaude_random(rng),
                             hdclaude_random(rng));
    ClosureData sampleData = ClosureData(CLOSURE_TYPE_PT_SAMPLE, vec3(0.0), V,
                                         point.shadingNormal, point.position, 1.0);
    hdclaude_material_shade(sampleData);
    vec3 L = hdclaude_bsdf.sampledL;

    if (!(dot(L, L) > 0.5))
    {
        pathThroughput.values[path] = vec3(0.0);
        pathRng.values[path] = rng;
        return;
    }
    L = normalize(L);

    ClosureData evalData = ClosureData(CLOSURE_TYPE_REFLECTION, L, V,
                                       point.shadingNormal, point.position, 1.0);
    hdclaude_material_shade(evalData);

    float pdf = hdclaude_bsdf.pdf;
    if (!(pdf > 0.0) || isnan(pdf) || isinf(pdf))
    {
        // A direction the sampler produced that the density says is
        // impossible: below the horizon, most often. Terminating is correct and
        // keeps the estimator unbiased.
        pathThroughput.values[path] = vec3(0.0);
        pathRng.values[path] = rng;
        return;
    }

    throughput *= hdclaude_bsdf.response / pdf;

    // Russian roulette after a few bounces, so a long dim path is terminated
    // with a compensating weight rather than traced to the depth limit.
    if (frame.bounce >= 2u)
    {
        float survival = clamp(max(throughput.x, max(throughput.y, throughput.z)),
                               0.05, 1.0);
        if (hdclaude_random(rng) > survival)
        {
            pathThroughput.values[path] = vec3(0.0);
            pathRng.values[path] = rng;
            return;
        }
        throughput /= survival;
    }

    if (!(dot(throughput, throughput) > 0.0))
    {
        pathThroughput.values[path] = vec3(0.0);
        pathRng.values[path] = rng;
        return;
    }

    // Offset to whichever side the scattered ray actually leaves on, so a
    // transmitted ray starts inside the surface rather than immediately
    // re-hitting the face it just passed through.
    pathOrigin.values[path] = hdclaude_offset_ray(
        point.position, dot(L, point.geometricNormal) > 0.0
                            ? point.geometricNormal
                            : -point.geometricNormal);
    pathDirection.values[path] = L;
    pathThroughput.values[path] = throughput;
    pathRng.values[path] = rng;

    // Compaction: only surviving paths enter the next bounce, so the following
    // dispatch is sized to them rather than to the original pixel count.
    uint next = atomicAdd(counters.nextActiveCount, 1u);
    nextActiveQueue.values[next] = path;
}
