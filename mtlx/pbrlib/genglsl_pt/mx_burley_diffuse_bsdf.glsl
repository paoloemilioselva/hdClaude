// hdClaude genglsl_pt override of burley_diffuse_bsdf.
//
// MaterialX 1.39.3's CLOSURE_TYPE_REFLECTION branch, verbatim, plus the density
// and the reconstruction guides. CLOSURE_TYPE_INDIRECT is dropped: it looks up
// a prefiltered environment, which is a rasteriser's approximation of the
// integral a path tracer computes exactly.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_diffuse.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_burley_diffuse_bsdf(ClosureData closureData, float weight, vec3 color, float roughness, vec3 N, inout BSDF bsdf)
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

    // ---- hdClaude: importance sampling -------------------------------------
    // Cosine-weighted, as for Oren-Nayar: Burley's retroreflective term
    // modulates the cosine distribution without moving its support, so a
    // matched proposal would cost more than it saves.
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
        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float LdotH = clamp(dot(L, normalize(L + V)), M_FLOAT_EPS, 1.0);

        bsdf.response = color * closureData.occlusion * weight * NdotL * M_PI_INV;
        bsdf.response *= mx_burley_diffuse(NdotV, NdotL, LdotH, roughness);

        // ---- hdClaude: density and reconstruction guides --------------------
        bsdf.pdf = dot(N, L) > 0.0 ? mx_pt_cosine_hemisphere_pdf(dot(N, L)) : 0.0;
        bsdf.isDelta = 0.0;
        bsdf.guideAlbedo = color * weight;
        bsdf.guideRoughness = 1.0;
    }
}
