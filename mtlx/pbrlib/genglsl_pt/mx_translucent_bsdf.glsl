// hdClaude genglsl_pt override of translucent_bsdf.
//
// MaterialX 1.39.3's CLOSURE_TYPE_REFLECTION branch, verbatim, plus the density
// and the reconstruction guides. CLOSURE_TYPE_INDIRECT is dropped.
//
// This is diffuse *transmission*: upstream inverts the normal because the light
// arrives from the far side of the surface. Sampling has to follow, so the
// cosine hemisphere is built around the inverted normal and the sampled
// direction goes into the surface rather than away from it. Sampling the front
// hemisphere here would produce a lobe that evaluates to zero everywhere it can
// be sampled, which reads as a black material rather than as a bug.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_translucent_bsdf(ClosureData closureData, float weight, vec3 color, vec3 N, inout BSDF bsdf)
{
    bsdf.throughput = vec3(0.0);

    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    // The colour of a diffuse lobe is an albedo, and an albedo above one is a
    // surface that returns more light than reaches it.
    //
    // `response / pdf` for this lobe is exactly `color * weight`, so a colour of
    // four multiplies the path by four at every scatter and thirty-two bounces
    // multiply it by 4^32. The OpenPBR Playground's `paper` does precisely
    // that: its `subsurface_color` is a texture through a `colorcorrect` node
    // with `gain = 4`, and at thirty-two bounces the scene rendered fireflies
    // of 10^5 (docs/implementation-notes.md, 2026-09-16).
    //
    // Clamped rather than honoured, because no amount of sampling makes a
    // divergent estimator converge, and because the transport this renderer
    // implements has no meaning for a reflectance above one. The same clamp is
    // already applied to a medium's single-scattering albedo in
    // `extend.comp.glsl`, for the same reason and with the same physics behind
    // it. What the asset authored is reported by
    // `scripts/check_scene_materials.py`, which reads the graph rather than the
    // shaded pixel.
    color = clamp(color, vec3(0.0), vec3(1.0));

    // Invert normal since we're transmitting light from the other side
    N = -N;

    // ---- hdClaude: reconstruction guides -----------------------------------
    // Properties of the surface and the view alone, so they are written before
    // the branch dispatch and every pass -- sampling included -- publishes the
    // same values (docs/dlss-integration.md 4).
    // The surface's own normal, not the inverted one this lobe scatters
    // around: the guide describes where the surface faces.
    bsdf.guideDiffuse = color * weight;
    bsdf.guideSpecular = vec3(0.0);
    bsdf.guideNormal = -N;
    bsdf.guideRoughness = 1.0;

    // ---- hdClaude: importance sampling -------------------------------------
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 X, Y;
        mx_pt_basis(N, X, Y);
        vec3 local = mx_pt_sample_cosine_hemisphere(hdclaude_sample_u.xy);
        bsdf.sampledL = mx_pt_to_world(local, X, Y, N);
        bsdf.isDelta = 0.0;
        return;
    }

    // ---- MaterialX 1.39.3 evaluation, unchanged -----------------------------
    //
    // Answered for TRANSMISSION as well, and TRANSMISSION is the one that
    // matters. A rasteriser evaluates every light with REFLECTION whichever side
    // it is on, so upstream never needed another; hdClaude's integrator asks
    // TRANSMISSION for a direction behind the normal -- which is every direction
    // this lobe samples and the only side it responds on. Answering REFLECTION
    // alone therefore discarded all of its samples and gave every light behind
    // it zero response: the chi-squared harness kept none of a million samples,
    // and thin-walled OpenPBR subsurface, which is built from this closure,
    // passed no light through at all.
    if (closureData.closureType == CLOSURE_TYPE_REFLECTION ||
        closureData.closureType == CLOSURE_TYPE_TRANSMISSION)
    {
        float NdotL = clamp(dot(N, L), 0.0, 1.0);
        bsdf.response = color * weight * NdotL * M_PI_INV;

        // ---- hdClaude: density and reconstruction guides --------------------
        // Measured against the inverted normal, matching the sample above.
        bsdf.pdf = NdotL > 0.0 ? mx_pt_cosine_hemisphere_pdf(NdotL) : 0.0;
        bsdf.isDelta = 0.0;
    }
}
