// The surface at a hit, rebuilt from a hit record and the instance's geometry.
//
// Shared by `shade`, which shades the point, and by `extend` and `shadow`,
// which rebuild a candidate's surface to ask a cut-out material's opacity
// whether the ray meets it at all. One reconstruction for both is what makes
// the point traversal tested the point shading sees.
//
// Included after path_state.glsl, whose instance table and buffer types it
// reads.

#ifndef HDCLAUDE_SURFACE_POINT_GLSL
#define HDCLAUDE_SURFACE_POINT_GLSL

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

#endif  // HDCLAUDE_SURFACE_POINT_GLSL
