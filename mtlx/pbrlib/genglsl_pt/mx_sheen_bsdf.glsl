// hdClaude genglsl_pt override of sheen_bsdf.
//
// MaterialX 1.39.3's CLOSURE_TYPE_REFLECTION branch, verbatim, plus the density
// and the reconstruction guides. CLOSURE_TYPE_INDIRECT is dropped.
//
// Note the throughput handling. Sheen is almost always the *top* of a layer,
// and `layer` chooses between its lobes using `top.throughput`, which sheen
// sets from its directional albedo. So the PT_SAMPLE branch must compute and
// publish `throughput` before it returns -- an early return that skipped it
// would leave the layer above selecting on a stale value, and the symptom would
// be a plausible-looking image with the wrong energy split.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_sheen.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_sheen_bsdf(ClosureData closureData, float weight, vec3 color, float roughness, vec3 N, int mode, inout BSDF bsdf)
{
    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    // Directional albedo is needed by every branch, including sampling, because
    // it is what `throughput` is derived from. Upstream computes it per branch.
    float sheenRoughness = (mode == 0) ? roughness : clamp(roughness, 0.01, 1.0);
    float dirAlbedo = (mode == 0)
                          ? mx_imageworks_sheen_dir_albedo(NdotV, sheenRoughness)
                          : mx_zeltner_sheen_dir_albedo(NdotV, sheenRoughness);
    bsdf.throughput = vec3(1.0 - dirAlbedo * weight);

    // ---- hdClaude: importance sampling -------------------------------------
    // A cosine hemisphere. Sheen's lobe is broad and grazing-weighted, so a
    // cosine proposal is imperfect but unbiased and cheap; a matched proposal
    // for the Zeltner form is a measurable improvement to revisit once the
    // furnace tests in docs/materialx-codegen.md 8 are in place.
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
    if (closureData.closureType == CLOSURE_TYPE_REFLECTION)
    {
        if (mode == 0)
        {
            vec3 H = normalize(L + V);

            float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
            float NdotH = clamp(dot(N, H), M_FLOAT_EPS, 1.0);

            vec3 fr = color * mx_imageworks_sheen_brdf(NdotL, NdotV, NdotH, sheenRoughness);

            // We need to include NdotL from the light integral here
            // as in this case it's not cancelled out by the BRDF denominator.
            bsdf.response = fr * NdotL * closureData.occlusion * weight;
        }
        else
        {
            vec3 fr = color * mx_zeltner_sheen_brdf(L, V, N, NdotV, sheenRoughness);
            bsdf.response = dirAlbedo * fr * closureData.occlusion * weight;
        }

        // ---- hdClaude: density and reconstruction guides --------------------
        bsdf.pdf = dot(N, L) > 0.0 ? mx_pt_cosine_hemisphere_pdf(dot(N, L)) : 0.0;
        bsdf.isDelta = 0.0;
        bsdf.guideAlbedo = color * dirAlbedo * weight;
        bsdf.guideRoughness = sheenRoughness;
    }
}
