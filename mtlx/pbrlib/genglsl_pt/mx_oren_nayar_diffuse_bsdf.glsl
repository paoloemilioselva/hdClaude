// hdClaude genglsl_pt override of oren_nayar_diffuse_bsdf.
//
// The CLOSURE_TYPE_REFLECTION branch is upstream's body, byte for byte, plus
// one added line writing bsdf.pdf. Only the PT_SAMPLE branch and that line are
// hdClaude's. Keeping the evaluation
// identical to upstream is deliberate: it makes any divergence from MaterialX
// visible in a diff against pbrlib/genglsl, and it makes an upstream MaterialX
// upgrade a mechanical merge rather than a re-derivation.
//
// The CLOSURE_TYPE_INDIRECT branch is dropped. It exists upstream to look up a
// prefiltered environment, which is a rasteriser's approximation of the
// integral a path tracer computes exactly. Leaving it in would give the
// generated code a second, disagreeing answer for the same quantity.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_diffuse.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_oren_nayar_diffuse_bsdf(ClosureData closureData, float weight, vec3 color, float roughness, vec3 N, bool energy_compensation, inout BSDF bsdf)
{
    bsdf.throughput = vec3(0.0);

    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    int closureType = closureData.closureType;

    // ---- hdClaude: reconstruction guides -----------------------------------
    // Properties of the surface and the view alone, so they are written before
    // the branch dispatch and every pass -- sampling included -- publishes the
    // same values (docs/dlss-integration.md 4).
    bsdf.guideDiffuse = color * weight;
    bsdf.guideSpecular = vec3(0.0);
    bsdf.guideNormal = N;
    bsdf.guideRoughness = 1.0;

    // ---- hdClaude: importance sampling -------------------------------------
    //
    // A cosine-weighted hemisphere is the correct proposal for every
    // Oren-Nayar roughness: the retroreflective lobe modulates the cosine
    // distribution but does not move its support, and a matched proposal would
    // buy less than it costs to evaluate.
    //
    // PT_SAMPLE returns only a direction. The caller evaluates the graph again
    // with CLOSURE_TYPE_REFLECTION at that direction to obtain both the
    // response and the density, so that a combinator above this node can mix
    // densities of the *same* direction. See lib/mx_closure_type.glsl.
    if (closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 X, Y;
        mx_pt_basis(N, X, Y);
        vec3 local = mx_pt_sample_cosine_hemisphere(hdclaude_sample_u.xy);

        bsdf.sampledL = mx_pt_to_world(local, X, Y, N);
        bsdf.isDelta = 0.0;
        return;
    }

    // ---- Upstream MaterialX evaluation, unchanged ---------------------------
    if (closureType == CLOSURE_TYPE_REFLECTION)
    {
        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float LdotV = clamp(dot(L, V), M_FLOAT_EPS, 1.0);

        vec3 diffuse = energy_compensation ?
                       mx_oren_nayar_compensated_diffuse(NdotV, NdotL, LdotV, roughness, color) :
                       mx_oren_nayar_diffuse(NdotV, NdotL, LdotV, roughness) * color;
        bsdf.response = diffuse * closureData.occlusion * weight * NdotL * M_PI_INV;

        // ---- hdClaude: density and reconstruction guides --------------------
        // Written here rather than in a separate query so that one code path
        // produces the response and the density that MIS pairs it with.
        bsdf.pdf = dot(N, L) > 0.0 ? mx_pt_cosine_hemisphere_pdf(dot(N, L)) : 0.0;
        bsdf.isDelta = 0.0;
    }
}
