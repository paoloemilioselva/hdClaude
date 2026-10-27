// Cut-out opacity, decided during traversal.
//
// MaterialX defines the `surface` node's `opacity` as the surface's cutout
// opacity: where it is below one the surface is partly not there, and a ray
// meets it with probability `opacity`. That is a question about *visibility*,
// so it is answered where visibility is -- in the candidate loop of every ray
// query that finds a surface or an occluder -- and not by a shading kernel,
// which only ever sees the surface a ray has already stopped at. Camera rays,
// bounce rays and shadow rays all ask it the same way, which is what keeps a
// leaf's shadow the shape of the leaf.
//
// The opacity of each cut-out material is a generated MaterialX program: one
// library holding every such material's graph is spliced into the traversal
// kernels when a scene has any (PathTracer::SetScene, OpacityLibraryDocument),
// and defines HDCLAUDE_TRAVERSAL_OPACITY. Without it every surface is present
// and nothing here costs anything, because no instance is then marked
// non-opaque and no triangle reaches the candidate loop on its account.
//
// Included after path_state.glsl.

#ifndef HDCLAUDE_CUTOUT_GLSL
#define HDCLAUDE_CUTOUT_GLSL

#include "surface_point.glsl"

/// The uniform number a candidate's opacity is compared with.
///
/// Hashed rather than drawn from the path's stream, for the reasons
/// `hdclaude_splat_coin` gives: traversal reports candidates in an order of the
/// driver's choosing, and may report one more than once unless told not to, so
/// a draw per candidate would make the path's every later number depend on the
/// tree walk -- and a primitive reported twice would get two chances to be
/// met. Hashed from the primitive, it gets one answer however often it is
/// asked.
///
/// `key` names the traversal: the path's state as the ray leaves, which differs
/// from bounce to bounce, so one leaf is an independent question for each ray
/// that crosses it.
float hdclaude_cutout_coin(uint key, int instance, int primitive)
{
    uint seed = key * 2654435761u;
    seed ^= uint(instance) * 3266489917u;
    seed ^= uint(primitive) * 668265263u;
    seed = hdclaude_pcg(seed);
    return float(hdclaude_pcg(seed) >> 8) * (1.0 / 16777216.0);
}

/// Whether a ray meets a candidate surface point.
///
/// `t` is along the world ray `origin + t * direction`; a triangle passes its
/// barycentrics and a curve passes zero, which hdclaude_reconstruct ignores for
/// a curve.
bool hdclaude_candidate_present(uint key, int instance, int primitive,
                                vec2 barycentrics, vec3 origin,
                                vec3 direction, float t)
{
#ifdef HDCLAUDE_TRAVERSAL_OPACITY
    InstanceGeometry geometry = instances.values[instance];
    uint material = hdclaude_material_of(geometry, primitive);
    // An opaque material on a placement that is marked non-opaque for another
    // of its materials: a GeomSubset, or a prototype shared between bindings.
    // Answered before the surface is rebuilt.
    if (!hdclaude_material_cutout(material))
    {
        return true;
    }

    SurfacePoint point = hdclaude_reconstruct(
        ivec4(instance, primitive, floatBitsToInt(barycentrics.x),
              floatBitsToInt(barycentrics.y)),
        direction, origin + direction * t);
    hdclaude_set_surface_hit(point.position, point.shadingNormal, point.tangent,
                             point.bitangent, point.objectPosition,
                             point.objectNormal, point.objectTangent,
                             point.objectBitangent, point.uv,
                             geometry.displayColor);

    // A probability, so one at or above one is always met and one at or below
    // zero never is -- the coin lies in [0, 1). The opacity is used as the
    // material computes it: an authored value outside [0, 1] behaves as its
    // nearer end, which is the only reading a probability has.
    return hdclaude_cutout_coin(key, instance, primitive) <
           hdclaude_material_opacity(material);
#else
    return true;
#endif
}

#endif  // HDCLAUDE_CUTOUT_GLSL
