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

    /// This material's Abbe number, or zero where it authors no dispersion.
    ///
    /// The one material property that does not reach the GPU inside the
    /// generated program. MaterialX 1.39.3 declares
    /// `transmission_dispersion_abbe_number` on `open_pbr_surface`, threads it
    /// into the generated function's signature, and never reads it, and
    /// `ND_dielectric_bsdf` has no input to receive it -- so the graph drops it
    /// before any closure can see it. hdClaude's material compiler reads it
    /// from the authored network instead and it arrives here, beside the
    /// program rather than inside it (docs/implementation-notes.md,
    /// 2026-09-07).
    float dispersionAbbe;

    /// Which bounce this dispatch is, counted from zero.
    ///
    /// A push constant rather than a field of the frame uniform, and that is what
    /// lets a whole sample be recorded into one command buffer. The uniform is
    /// written by the *host*, so a value that varied per bounce forced a submit
    /// and a full wait between every pair of bounces -- 288 of them per gallery
    /// frame -- and every dispatch in a batched buffer would otherwise have read
    /// whichever value happened to be written last.
    uint bounce;

    /// One when this material is a thin-walled sheet, read from the document
    /// because the MaterialX graph does not carry it to the transmission lobe.
    /// Such a surface has no interior, so a transmission through it enters no
    /// medium, and its dielectric transmits without refracting.
    uint thinWalled;
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

    // A curve has no triangle to interpolate over. Its surface is the segment
    // itself, and everything the shading needs -- the normal, the frame, the
    // texture coordinate -- follows from where on that segment the ray landed.
    if (geometry.segments != 0ul)
    {
        SegmentBuffer curveSegments = SegmentBuffer(geometry.segments);
        uint base = uint(record.y) * 10u;
        vec3 pa = vec3(curveSegments.values[base + 0u],
                       curveSegments.values[base + 1u],
                       curveSegments.values[base + 2u]);
        float ra = curveSegments.values[base + 3u];
        vec3 pb = vec3(curveSegments.values[base + 5u],
                       curveSegments.values[base + 6u],
                       curveSegments.values[base + 7u]);
        float rb = curveSegments.values[base + 8u];
        // How far along the whole strand each end of this segment is. Carried
        // on the segment because it cannot be recovered from one: a curve's
        // texture coordinate runs root to tip, and a segment measuring only
        // itself would give every strand a sawtooth.
        float va = curveSegments.values[base + 4u];
        float vb = curveSegments.values[base + 9u];

        // The hit in object space, which is the space the segments are in.
        vec3 objectPoint = vec4(hitPosition, 1.0) * geometry.worldToObject;

        float along = 0.0;
        vec3 objectNormal =
            hdclaude_segment_normal(objectPoint, pa, ra, pb, rb, along);

        mat3 normalMatrix = transpose(hdclaude_linear(geometry.worldToObject));
        point.position = hitPosition;
        point.geometricNormal = normalize(normalMatrix * objectNormal);
        // A curve carries no authored shading normal: the tube *is* the
        // surface, so the geometric normal is the shading normal. Nothing is
        // being approximated away here, unlike a swept tube whose facets each
        // carried a normal of their own.
        point.shadingNormal = point.geometricNormal;
        point.frontGeometricNormal =
            dot(point.geometricNormal, rayDirection) > 0.0
                ? -point.geometricNormal
                : point.geometricNormal;

        // The axis is the tangent, which is what a hair shading model wants:
        // an anisotropic closure orients along the strand rather than around
        // it.
        vec3 objectAxis = pb - pa;
        vec3 worldAxis = hdclaude_linear(geometry.objectToWorld) * objectAxis;
        point.tangent = normalize(
            dot(worldAxis, worldAxis) > 1.0e-20
                ? worldAxis - point.shadingNormal *
                                  dot(point.shadingNormal, worldAxis)
                : hdclaude_any_perpendicular(point.shadingNormal));
        point.bitangent = normalize(cross(point.shadingNormal, point.tangent));

        // v runs along the strand and u around it, which is the convention the
        // swept tube used and so the one any material already authored against
        // expects.
        vec3 around = objectPoint - mix(pa, pb, along);
        vec3 axis = normalize(objectAxis);
        vec3 side, up;
        hdclaude_light_basis(axis, side, up);
        float angle = atan(dot(around, up), dot(around, side));
        point.uv = vec2(angle * (0.5 / 3.14159265358979323846) + 0.5,
                        mix(va, vb, along));

        point.objectPosition = objectPoint;
        point.objectNormal = objectNormal;
        point.objectTangent = normalize(objectAxis);
        point.objectBitangent = normalize(cross(objectNormal, objectAxis));
        return point;
    }

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
    // The path's own hero wavelengths, not a fixed quartet. Nothing reads this
    // yet, which is exactly why it was able to sit here wrong: a closure that
    // needs to know which wavelengths it is being asked about -- a dispersive
    // dielectric is the first that will -- would have been handed the same four
    // numbers for every path, and the error would have looked like a dispersion
    // model that does not work rather than like an input that was never
    // connected.
    hdclaude_wavelengths = lambda;
    hdclaude_dispersion_abbe = shadeParams.dispersionAbbe;
    hdclaude_thin_walled = float(shadeParams.thinWalled);
    // The true geometric normal, so a closure can tell which side of the
    // interface it is on. The shading normal cannot answer that: near a
    // silhouette it tilts past the horizon on a perfectly opaque object.
    hdclaude_geometric_normal = point.geometricNormal;
    // Whether this path is inside a dense medium, which decides which way round
    // a dielectric's relative index goes. A closure cannot know it; only the
    // transmission events that carried the path here do.
    hdclaude_inside_medium = pathMedium.values[2u * path + 1u].w;

    // --- Dispersion: collapse the packet onto its hero wavelength ------------
    //
    // A dispersive interface sends every wavelength somewhere else, and a path
    // is one direction. So the packet stops being four correlated estimates the
    // moment it meets one: the hero lane is refracted by its own index and the
    // other three are terminated, because there is no direction they could
    // honestly be carried along.
    //
    // Terminating alone would be four times too dark. The film averages the
    // four lanes -- it divides by `4 * p(lambda_hero)`, and every lane shares
    // the hero's density -- so three empty lanes make an average of a quarter of
    // one estimate. Scaling the survivor by the lane count restores the single
    // wavelength estimator `CMF(lambda_0) * L_0 / p`, which is unbiased and
    // simply four times noisier. That is the true cost of dispersion, and it is
    // paid only by paths that touch a dispersive material.
    //
    // Radiance already gathered in the other lanes is untouched: it was
    // estimated before the collapse and is still theirs.
    //
    // Done before the emission and next-event blocks rather than at the scatter,
    // because the closure below is about to be evaluated at the hero index and
    // everything it returns is that wavelength's alone.
    if (shadeParams.dispersionAbbe > 0.0 && pathHeroOnly.values[path] == 0u)
    {
        throughput.x *= float(HDCLAUDE_SPECTRAL_LANES);
        throughput.y = 0.0;
        throughput.z = 0.0;
        throughput.w = 0.0;
        pathHeroOnly.values[path] = 1u;
    }

    // --- Reconstruction guides ----------------------------------------------
    //
    // The primary surface's, so only on the first bounce, and asked for in a
    // pass of their own: the closures publish their guides whichever pass runs,
    // but the scattering pass below does not run at all on the last bounce, and
    // a render of one bounce would then have none. The sample value is fixed
    // rather than drawn, so this consumes no random numbers, and running it
    // changes nothing downstream: with this block compiled in but never taken,
    // the subdivision scene's ray hash equals the hash with it taken. Both
    // differ from the kernel before the block existed, which is the driver
    // compiling a different program -- the floating-point bits of unrelated
    // expressions move when the code around them does -- and not this pass.
    if (shadeParams.bounce == 0u)
    {
        hdclaude_sample_u = vec3(0.5);
        ClosureData guideData = ClosureData(CLOSURE_TYPE_PT_SAMPLE, vec3(0.0), V,
                                            point.shadingNormal, point.position,
                                            1.0);
        hdclaude_material_shade(guideData);

        // The closures' shading normal, which is where a normal map lands. A
        // material with no scattering lobe -- an emitter alone -- reports none,
        // and its surface's normal is then the interpolated one, turned to face
        // the viewer as every closure turns its own.
        vec3 normal = hdclaude_bsdf.guideNormal;
        normal = dot(normal, normal) > 1.0e-12
                     ? normalize(normal)
                     : faceforward(point.shadingNormal, rayDirection,
                                   point.shadingNormal);
        float roughness = sqrt(clamp(hdclaude_bsdf.guideRoughness, 0.0, 1.0));

        uint pixel = pathPixel.values[path];
        guideSurface.values[3u * pixel] = vec4(normal, roughness);
        guideSurface.values[3u * pixel + 1u] =
            vec4(max(hdclaude_bsdf.guideDiffuse, vec3(0.0)), 0.0);
        guideSurface.values[3u * pixel + 2u] =
            vec4(max(hdclaude_bsdf.guideSpecular, vec3(0.0)), 0.0);

        // The probe the specular hit distance is measured along: the view
        // reflected about the normal the closures answer to, which is the
        // direction the specular lobe is centred on. Started on the side of the
        // true surface it leaves by, so a normal map that tilts it below the
        // triangle does not start it inside the object.
        vec3 reflected = normalize(reflect(rayDirection, normal));
        vec3 side = dot(reflected, point.frontGeometricNormal) >= 0.0
                        ? point.frontGeometricNormal
                        : -point.frontGeometricNormal;
        guideSpecularRay.values[2u * pixel] =
            vec4(hdclaude_offset_ray(point.position, side), 1.0);
        guideSpecularRay.values[2u * pixel + 1u] = vec4(reflected, 65504.0);
    }

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
                //
                // A light whose geometry is not rendered is in the same
                // position as the stand-in sun: no scattered ray can find it,
                // so there is no second strategy to share with and next-event
                // estimation takes the whole contribution. Weighting it anyway
                // would throw away the share of a strategy that cannot happen,
                // and every hidden light would be too dark by exactly that
                // share.
                bool hittable = !sunSample;
                if (hittable && emitter < frame.lightCount)
                {
                    hittable =
                        hdclaude_light_geometry_visible(lights.values[emitter]);
                }

                float weight = 1.0;
                if (hittable)
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
                vec4 lightResponse =
                    hdclaude_upsample(hdclaude_bsdf.response, lambda);

                // The interior the shadow ray crosses, if it crosses one.
                //
                // A shadow ray that reaches a light without meeting a surface
                // has not left whatever volume it started in, so when that is
                // an interior the light arrives attenuated by it -- the same
                // `exp(-sigma_t * d)` a scattered path's walk returns in
                // expectation for a flight with no collision on it. Only two
                // ways for a shadow ray to start inside one exist. The path is
                // already in an interior and the light is on the side it came
                // from; or the light is behind a transmissive surface the ray
                // *enters*, and it enters the interior of the lobe that carries
                // it, which the evaluation has just chosen as the sampling pass
                // would have (hdclaude_select_medium). A ray leaving an
                // interior, or one that never enters, is in no medium at all.
                //
                // Entering is decided exactly as the scattered path decides it
                // below -- by the geometric normal facing the view -- and not
                // from the path's record of where it is, which does not nest: a
                // path that has passed through an ice cube in a glass of juice
                // is recorded as in vacuum while it is still in juice.
                //
                // Only for a light that casts shadows. A light authored with
                // `shadow:enable` false is one no object blocks, so its
                // contribution lands with no shadow ray and nothing establishes
                // where along the line the interior ends; attenuating it over
                // the whole distance to the light treated the medium as reaching
                // all the way there. An interior's absorption is part of how an
                // object shadows, and such a light is not shadowed. The OpenPBR
                // Playground's moon, sun and LED lights are authored that way,
                // and its octopus lost most of their light before this.
                //
                // This used to be missing, and it only matters when the shadow
                // ray is unoccluded -- a light *inside* the medium, since any
                // light outside it is behind the far wall. There it mattered in
                // full: next-event estimation delivered the light unattenuated
                // while the scattered path attenuated it, and a light inside an
                // absorbing slab seen through a rough interface rendered with
                // the estimate's share of it as though the slab were clear.
                {
                    vec4 pathInterior = pathMedium.values[2u * path + 1u];
                    vec3 crossed = vec3(0.0);
                    if (lightInFront && pathInterior.w > 1.5)
                    {
                        crossed = pathMedium.values[2u * path + 0u].xyz;
                    }
                    else if (!lightInFront &&
                             dot(point.geometricNormal, V) > 0.0 &&
                             shadeParams.thinWalled == 0u &&
                             hdclaude_bsdf.mediumKind != HDCLAUDE_MEDIUM_NONE)
                    {
                        crossed = hdclaude_bsdf.mediumExtinction;
                    }
                    if (lightSample.castsShadows &&
                        max(max(crossed.x, crossed.y), crossed.z) > 0.0)
                    {
                        // Capped so an environment sample's unbounded distance
                        // stays finite, and zero extinction stays zero rather
                        // than becoming a product with infinity.
                        vec4 sigmaT = hdclaude_lane_extinction(crossed, lambda);
                        lightResponse *=
                            exp(-sigmaT * min(lightSample.distance, 1.0e30));
                    }
                }

                vec4 contribution =
                    throughput *
                    lightResponse *
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
    if (shadeParams.bounce + 1u >= frame.maxBounces)
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

    // The interior the selected lobe encloses, read here and not later.
    //
    // It has to be read from the *sampling* pass, because that is the pass in
    // which a combinator chose a lobe, and the medium belongs to the lobe that
    // carries the path through the interface rather than to the material. Both
    // `standard_surface` and `open_pbr_surface` instantiate `subsurface_bsdf`
    // and `anisotropic_vdf` unconditionally and gate them downstream with a
    // `mix`, so every one of these materials describes two interiors and enters
    // at most one. The evaluation pass below does not select, and would leave
    // this saying whatever ran last.
    vec3 mediumExtinction = hdclaude_bsdf.mediumExtinction;
    vec3 mediumAlbedo = hdclaude_bsdf.mediumAlbedo;
    float mediumAnisotropy = hdclaude_bsdf.mediumAnisotropy;
    float mediumKind = hdclaude_bsdf.mediumKind;

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
    // A thin-walled sheet has no interior (OpenPBR, "Thin-walled mode"), so a
    // path that passes through one is in whatever it was in before. Whatever
    // volume the graph attached to its transmission lobe describes a bulk the
    // specification says is not there.
    if (scatterClosure == CLOSURE_TYPE_TRANSMISSION &&
        shadeParams.thinWalled == 0u)
    {
        bool goingIn = dot(point.geometricNormal, V) > 0.0;
        // Subsurface arrives here under the same name as a volume, because a
        // random walk beneath a surface and one inside a volume are the same
        // walk. `subsurface_bsdf` publishes an interior in the same terms
        // `anisotropic_vdf` does, so there is one mechanism and no priority to
        // decide between them: the lobe that was selected brought its own.
        //
        // Carried in the terms the closure published it in, including which
        // terms those are. The two closures describe an interior differently --
        // `anisotropic_vdf` by a pair of coefficients, `subsurface_bsdf` by the
        // colour that comes back out -- and only one of them needs a nonlinear
        // relation applied per wavelength, so the traversal kernel has to be
        // told which it is holding. See shaders/extend.comp.glsl.
        // `w` says two things at once, and they are genuinely different
        // questions: whether the path is *inside* -- which a clear dielectric
        // answers yes to, and which decides the relative index a closure reads
        // -- and whether there is an interior to transport through, which it
        // answers no to. Conflating them would either stop glass seeing the
        // index it is looking through, or make every refraction enter a medium
        // that is not there and be killed the moment it reached open geometry.
        pathMedium.values[2u * path + 0u] =
            goingIn ? vec4(mediumExtinction, mediumAnisotropy) : vec4(0.0);
        pathMedium.values[2u * path + 1u] =
            goingIn ? vec4(mediumAlbedo, HDCLAUDE_INSIDE + mediumKind)
                    : vec4(0.0);
    }

    // What the environment kernel weighs against, if this ray misses. A delta
    // closure reports no finite density and next-event estimation skipped it,
    // so it stores zero and takes the environment in full.
    pathScatterPdf.values[path] =
        hdclaude_bsdf.isDelta < 0.5 ? pdf : 0.0;

    // Russian roulette after a few bounces, so a long dim path is terminated
    // with a compensating weight rather than traced to the depth limit.
    if (shadeParams.bounce >= 2u)
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
