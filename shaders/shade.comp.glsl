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
    /// distinct material, and each is dispatched over its own group of the
    /// sorted queue -- so an invocation here always has a path to shade, and
    /// the dispatch is sized to that group rather than to the frame
    /// (docs/wavefront-integrator.md 3).
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
    /// The texture-space frame: `tangent` runs along increasing u and
    /// `bitangent` along increasing v, both made orthogonal to the shading
    /// normal. A tangent-space normal map is defined against exactly these
    /// axes, so a frame taken from anywhere else -- a triangle edge, a world
    /// axis -- applies every map at a rotation that changes from triangle to
    /// triangle.
    vec3 tangent;
    vec3 bitangent;
    vec2 uv;

    /// The same point in the instance's object space.
    ///
    /// Carried rather than derived at use: a 3D procedural pattern is authored
    /// against object space so that it stays put when the object moves, and a
    /// material that reads it must get a real value. Before these existed the
    /// generated setter left the object-space members of its geometry struct
    /// unassigned, and every material with a `fractal3d`, `noise3d` or
    /// `worleynoise3d` node shaded from undefined memory.
    vec3 objectPosition;
    vec3 objectNormal;
    vec3 objectTangent;
    vec3 objectBitangent;
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
    mat3 normalMatrix = transpose(hdclaude_linear(geometry.worldToObject));
    point.geometricNormal = normalize(normalMatrix * objectGeometric);

    // The object-space shading normal, interpolated over the triangle. Kept in
    // a local as well as on the point: the object-space frame below wants it,
    // and re-reading three buffer entries to rebuild it is the kind of
    // duplication that drifts.
    vec3 objectShading = objectGeometric;
    if (geometry.normals != 0ul)
    {
        NormalBuffer normals = NormalBuffer(geometry.normals);
        if (geometry.normalsPerCorner != 0u)
        {
            // Face-varying: three normals belong to this triangle alone, in
            // the order its indices were written. A crease is authored exactly
            // this way, so indexing these by vertex would average the two
            // sides of it back together.
            uint corner = uint(record.y) * 3u;
            objectShading = w * normals.values[corner + 0u] +
                            u * normals.values[corner + 1u] +
                            v * normals.values[corner + 2u];
        }
        else
        {
            objectShading = w * normals.values[i0] + u * normals.values[i1] +
                            v * normals.values[i2];
        }
        // A degenerate interpolation -- opposed authored normals, or a vertex
        // left at zero by a mesh with no adjacency there -- would normalize to
        // a NaN and take the whole path with it.
        if (!(dot(objectShading, objectShading) > 1.0e-20))
        {
            objectShading = objectGeometric;
        }
        objectShading = normalize(objectShading);
    }
    point.shadingNormal = normalize(normalMatrix * objectShading);

    // The viewer-facing copy, for ray offsets and light-side tests. The true
    // normals above are left alone; see the note on SurfacePoint.
    point.frontGeometricNormal = dot(point.geometricNormal, rayDirection) > 0.0
                                     ? -point.geometricNormal
                                     : point.geometricNormal;

    // The three corners' coordinates, not just the interpolated one: the
    // tangent frame below is the rate at which the surface moves per unit of
    // u and of v, and that can only be read off the triangle as a whole.
    vec2 uv0 = vec2(0.0, 0.0);
    vec2 uv1 = vec2(1.0, 0.0);
    vec2 uv2 = vec2(0.0, 1.0);
    if (geometry.uvs != 0ul)
    {
        UvBuffer uvs = UvBuffer(geometry.uvs);
        if (geometry.uvsPerCorner != 0u)
        {
            // Face-varying: three coordinates belong to this triangle alone,
            // in the order its indices were written.
            uint corner = uint(record.y) * 3u;
            uv0 = uvs.values[corner + 0u];
            uv1 = uvs.values[corner + 1u];
            uv2 = uvs.values[corner + 2u];
        }
        else
        {
            uv0 = uvs.values[i0];
            uv1 = uvs.values[i1];
            uv2 = uvs.values[i2];
        }
    }
    // The defaults above are the barycentric parameterisation, so a mesh with
    // no coordinates falls out of the same arithmetic with (u, v) as its
    // surface parameters rather than needing a branch of its own.
    point.uv = w * uv0 + u * uv1 + v * uv2;

    // --- The tangent frame ---------------------------------------------------
    //
    // dP/du and dP/dv, solved from how position and texture coordinate vary
    // together across this triangle. This is what a tangent-space normal map
    // is defined against: its x perturbs the surface along increasing u and
    // its y along increasing v. A tangent taken from an edge instead -- which
    // is what this did -- is a different rotation on every triangle, so a
    // normal map becomes per-triangle noise, and the two triangles of a quad
    // disagree by roughly ninety degrees. Every chess piece and every OpenPBR
    // Playground surface reads its normal map through this frame.
    vec3 e1 = p1 - p0;
    vec3 e2 = p2 - p0;
    vec2 duv1 = uv1 - uv0;
    vec2 duv2 = uv2 - uv0;
    float uvDeterminant = duv1.x * duv2.y - duv2.x * duv1.y;

    vec3 objectDpDu;
    vec3 objectDpDv;
    if (abs(uvDeterminant) > 1.0e-20)
    {
        float inverse = 1.0 / uvDeterminant;
        objectDpDu = (duv2.y * e1 - duv1.y * e2) * inverse;
        objectDpDv = (duv1.x * e2 - duv2.x * e1) * inverse;
    }
    else
    {
        // A collapsed UV triangle: the parameterisation says nothing about
        // direction here, so fall back to the edge. Anisotropy then rotates
        // with the surface, which is the best available answer; a normal map
        // on such a triangle has no defined orientation to begin with.
        objectDpDu = e1;
        objectDpDv = cross(objectGeometric, e1);
    }

    mat3 objectToWorldLinear = hdclaude_linear(geometry.objectToWorld);
    vec3 dpdu = objectToWorldLinear * objectDpDu;
    vec3 dpdv = objectToWorldLinear * objectDpDv;

    // Orthogonalised against the *shading* normal, because that is the third
    // axis MaterialX's normalmap builds its frame from; leaving the tangent in
    // the geometric plane tilts every perturbed normal by the angle between
    // the two.
    point.tangent = dpdu - point.shadingNormal * dot(point.shadingNormal, dpdu);
    if (!(dot(point.tangent, point.tangent) > 1.0e-20))
    {
        // dP/du parallel to the normal, or degenerate; any orthogonal
        // direction will do.
        vec3 fallback = abs(point.shadingNormal.z) < 0.9 ? vec3(0.0, 0.0, 1.0)
                                                         : vec3(1.0, 0.0, 0.0);
        point.tangent = cross(fallback, point.shadingNormal);
    }
    point.tangent = normalize(point.tangent);

    // Handedness read from dP/dv rather than assumed. A mirrored UV island --
    // which is how half of a symmetric asset is normally laid out, both chess
    // pieces included -- runs v the other way round, and a bitangent fixed at
    // cross(N, T) inverts every mapped detail on exactly those islands.
    vec3 bitangent = cross(point.shadingNormal, point.tangent);
    point.bitangent = dot(bitangent, dpdv) < 0.0 ? -bitangent : bitangent;

    // The object-space frame. The position is interpolated from the vertices
    // rather than taken back through the inverse transform, so it is exact at
    // the scales where a world position has already lost precision; the
    // directions come back through the transform because that is all there is.
    point.objectPosition = w * p0 + u * p1 + v * p2;
    point.objectNormal = objectShading;
    mat3 worldToObjectLinear = hdclaude_linear(geometry.worldToObject);
    point.objectTangent = normalize(worldToObjectLinear * point.tangent);
    point.objectBitangent = normalize(worldToObjectLinear * point.bitangent);

    return point;
}

void main()
{
    // This material's group of the sorted queue. The sort has already
    // established that every path in it hit geometry and that this material
    // shades it, so neither test is repeated here. The bound is still needed:
    // a group is a whole number of workgroups, so the last one runs wide.
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= hdclaude_material_count(shadeParams.materialId))
    {
        return;
    }
    uint path = materialQueue.values[
        hdclaude_material_offset(shadeParams.materialId) + slot];

    ivec4 record = hits.values[path];

    vec4 throughput = pathThroughput.values[path];
    if (dot(throughput, throughput) <= 0.0)
    {
        return;
    }
    vec4 lambda = pathWavelengths.values[path];

    vec3 rayDirection = pathDirection.values[path];
    vec3 hitPosition = pathOrigin.values[path];
    SurfacePoint point = hdclaude_reconstruct(record, rayDirection, hitPosition);

    vec3 V = -rayDirection;
    uint rng = pathRng.values[path];

    // Hand the geometry to the generated material. The setter assigns only the
    // members this material actually reads.
    hdclaude_set_surface_hit(point.position, point.shadingNormal, point.tangent,
                             point.bitangent, point.objectPosition,
                             point.objectNormal, point.objectTangent,
                             point.objectBitangent, point.uv);
    hdclaude_wavelengths = vec4(450.0, 550.0, 600.0, 650.0);

    // --- Emission -----------------------------------------------------------
    ClosureData emissionData = ClosureData(CLOSURE_TYPE_EMISSION, vec3(0.0), V,
                                           point.shadingNormal, point.position, 1.0);
    hdclaude_material_shade(emissionData);
    pathRadiance.values[path] +=
        throughput * hdclaude_upsample_emission(hdclaude_emission, lambda);

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
    // The analytic lights are hittable, so this estimate is weighed against
    // the scattered ray that may reach the same light. The stand-in sun is the
    // exception: it is a delta emitter with no solid angle to hit, added as a
    // disc on the camera ray alone, and weighing it would discard the half of
    // its contribution that has no second strategy to recover it.
    {
        // One emitter per bounce, chosen uniformly among the analytic lights
        // and the environment. The environment is an emitter here rather than
        // something a scattered ray stumbles into: in an enclosed set almost
        // no ray reaches it, and a sky that only arrives through a chain of
        // surviving bounces lights the room dimly and noisily.
        uint emitters = hdclaude_emitter_count();
        uint emitter = min(uint(hdclaude_random(rng) * float(emitters)),
                           emitters - 1u);
        float selectionPdf = 1.0 / float(emitters);

        vec2 lightU = vec2(hdclaude_random(rng), hdclaude_random(rng));

        LightSample lightSample;
        bool environmentSample = emitter == frame.lightCount;
        bool sunSample = emitter > frame.lightCount;
        if (sunSample)
        {
            // The stand-in sun, one option among the emitters rather than an
            // extra sample of its own. A path emits exactly one shadow ray per
            // bounce -- the shadow kernel adds contributions without atomics on
            // that basis, and the shadow queue is sized on it -- so an emitter
            // that is sampled *in addition* races and overflows rather than
            // adding light.
            lightSample.direction = normalize(frame.sunDirection.xyz);
            lightSample.distance = 1.0e30;
            lightSample.radiance = frame.sunRadiance.rgb;
            // A delta emitter: there is no solid angle to divide by, so the
            // estimator's density is one and the selection probability is the
            // whole of it.
            lightSample.pdf = 1.0;
            lightSample.castsShadows = true;
            lightSample.colorTemperature = 0.0;
            lightSample.temperatureScale = 1.0;
        }
        else if (environmentSample)
        {
            // Sampled from the map's own luminance, so the shadow rays go
            // where the light is. A dome's radiance is concentrated almost
            // entirely in a window or a sun covering a fraction of a percent
            // of the sphere; found uniformly, that arrives as fireflies rather
            // than as light. The density stays a function of direction alone,
            // which is what lets the environment kernel weigh a scattered ray
            // against this same strategy.
            EnvironmentSample environmentDirection =
                hdclaude_sample_environment(lightU);
            lightSample.direction = environmentDirection.direction;
            lightSample.distance = 1.0e30;
            lightSample.radiance = hdclaude_environment(lightSample.direction);
            lightSample.pdf = environmentDirection.pdf;
            lightSample.castsShadows = true;
            lightSample.colorTemperature = frame.environmentTemperature;
            lightSample.temperatureScale = frame.environmentTemperatureScale;
        }
        else
        {
            lightSample = hdclaude_sample_light(emitter, point.position, lightU);
        }

        // Which side of the surface the light lies on decides which closure
        // can carry it, exactly as it does for a scattered direction. A light
        // in front is a reflection; a light behind is a *transmission*, and
        // asking the reflection closure about it -- or refusing to ask at all,
        // which is what this used to do -- leaves a refracting surface with no
        // estimate of the light it is looking straight through. Glass then has
        // one strategy where every opaque surface has two, and is
        // correspondingly loud.
        bool lightInFront =
            dot(lightSample.direction, point.frontGeometricNormal) > 0.0;
        vec3 shadowNormal = lightInFront ? point.frontGeometricNormal
                                         : -point.frontGeometricNormal;

        if (lightSample.pdf > 0.0)
        {
            hdclaude_sample_u = vec3(hdclaude_random(rng), hdclaude_random(rng),
                                     hdclaude_random(rng));
            ClosureData lightData = ClosureData(
                lightInFront ? CLOSURE_TYPE_REFLECTION
                             : CLOSURE_TYPE_TRANSMISSION,
                lightSample.direction, V, point.shadingNormal, point.position,
                1.0);
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
                // Every emitter a scattered ray can also find takes the
                // balance heuristic's share against the closure's own density
                // at this direction. That is the environment and, since the
                // lights became opaque emitters intersected in closed form,
                // the analytic lights as well.
                float weight = 1.0;
                if (!sunSample)
                {
                    weight = hdclaude_mis_weight(lightSample.pdf * selectionPdf,
                                                 hdclaude_bsdf.pdf);
                }

                // The closure's response is a reflectance and the light's
                // radiance is an emission, and the two are upsampled
                // differently: a reflectance is the bare spectrum, an emission
                // is that spectrum times the illuminant its RGB was authored
                // against. Multiplying two reflectances would render every lit
                // surface under an equal-energy sky nobody authored.
                vec4 contribution =
                    throughput *
                    hdclaude_upsample(hdclaude_bsdf.response, lambda) *
                    hdclaude_upsample_emission(lightSample.radiance, lambda,
                                               lightSample.colorTemperature,
                                               lightSample.temperatureScale) *
                    weight / (lightSample.pdf * selectionPdf);

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
                            // Offset along the side the ray leaves on, or a
                            // transmitted shadow ray starts on the wrong side
                            // of the surface it just passed through and is
                            // occluded by it immediately.
                            ray.origin =
                                hdclaude_offset_ray(point.position, shadowNormal);
                            ray.direction = lightSample.direction;
                            ray.contribution = contribution;
                            // Stop just short of the light so the light's own
                            // backing geometry, if the scene has any, does not
                            // occlude it.
                            ray.maxDistance = lightSample.distance * 0.9999;
                            ray.path = path;
                            ray.pad0 = 0u; ray.pad1 = 0u;
                            shadowRays.values[index] = ray;
                        }
                    }
                }
            }
        }
    }
    // --- Scatter -------------------------------------------------------------
    if (frame.bounce + 1u >= frame.maxBounces)
    {
        pathThroughput.values[path] = vec4(0.0);
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
        pathThroughput.values[path] = vec4(0.0);
        pathRng.values[path] = rng;
        return;
    }
    L = normalize(L);

    // Which side the sampled direction left on decides which closure evaluates
    // it. A refraction crosses the surface, and asking the reflection branch
    // for a direction below its horizon gets zero response and zero density --
    // the path is then terminated as impossible, and a transmissive material
    // renders black no matter how many bounces it is given. That is what the
    // glass shader ball did.
    int scatterClosure =
        dot(L, point.shadingNormal) * dot(V, point.shadingNormal) > 0.0
            ? CLOSURE_TYPE_REFLECTION
            : CLOSURE_TYPE_TRANSMISSION;

    ClosureData evalData = ClosureData(scatterClosure, L, V,
                                       point.shadingNormal, point.position, 1.0);
    hdclaude_material_shade(evalData);

    float pdf = hdclaude_bsdf.pdf;
    if (!(pdf > 0.0) || isnan(pdf) || isinf(pdf))
    {
        // A direction the sampler produced that the density says is
        // impossible: below the horizon, most often. Terminating is correct and
        // keeps the estimator unbiased.
        pathThroughput.values[path] = vec4(0.0);
        pathRng.values[path] = rng;
        return;
    }

    throughput *= hdclaude_upsample(hdclaude_bsdf.response, lambda) / pdf;

    // --- Crossing into or out of an interior medium --------------------------
    //
    // Only a transmission changes which volume the path is in, and which way it
    // crossed decides whether the medium is entered or left. `V` points back
    // along the incoming ray, so a geometric normal facing it means the ray
    // arrived from outside and this transmission goes *in*.
    //
    // The closure has just been evaluated, so a material with a volume has
    // published its coefficient in `hdclaude_medium_absorption`; a material
    // without one leaves it zero, which is vacuum and costs nothing. Leaving is
    // unconditional: the far side of a closed object is whatever contains it,
    // and nesting media is a scope this does not claim.
    if (scatterClosure == CLOSURE_TYPE_TRANSMISSION)
    {
        bool goingIn = dot(point.geometricNormal, V) > 0.0;
        pathMedium.values[path] =
            goingIn ? vec4(hdclaude_medium_absorption, 0.0) : vec4(0.0);
    }

    // What the environment kernel weighs against, if this ray misses. A delta
    // closure reports no finite density and next-event estimation skipped it,
    // so it stores zero and takes the environment in full.
    pathScatterPdf.values[path] =
        hdclaude_bsdf.isDelta < 0.5 ? pdf : 0.0;

    // Russian roulette after a few bounces, so a long dim path is terminated
    // with a compensating weight rather than traced to the depth limit.
    if (frame.bounce >= 2u)
    {
        float survival = clamp(max(max(throughput.x, throughput.y),
                                   max(throughput.z, throughput.w)),
                               0.05, 1.0);
        if (hdclaude_random(rng) > survival)
        {
            pathThroughput.values[path] = vec4(0.0);
            pathRng.values[path] = rng;
            return;
        }
        throughput /= survival;
    }

    if (!(dot(throughput, throughput) > 0.0))
    {
        pathThroughput.values[path] = vec4(0.0);
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
