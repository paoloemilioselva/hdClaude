// hdClaude genglsl_pt override of conductor_bsdf.
//
// As with the diffuse override, the CLOSURE_TYPE_REFLECTION branch is
// upstream's body unchanged, plus added lines for the density and guides.
//
// The GGX sampling itself is upstream's: `mx_ggx_importance_sample_VNDF` and
// `mx_ggx_VNDF_reflection_PDF` already exist in
// pbrlib/genglsl/lib/mx_microfacet_specular.glsl and are used verbatim. A
// second GGX implementation that disagreed with MaterialX's own would be a
// fidelity bug that no test of hdClaude against itself could find.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_specular.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_conductor_bsdf(ClosureData closureData, float weight, vec3 ior_n, vec3 ior_k, vec2 roughness, bool retroreflective, float thinfilm_thickness, float thinfilm_ior, vec3 N, vec3 X, int distribution, inout BSDF bsdf)
{
    bsdf.throughput = vec3(0.0);

    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    V = retroreflective ? reflect(-V, N) : V;
    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    FresnelData fd = mx_init_fresnel_conductor(ior_n, ior_k, thinfilm_thickness, thinfilm_ior);

    vec2 safeAlpha = clamp(roughness, M_FLOAT_EPS, 1.0);
    float avgAlpha = mx_average_alpha(safeAlpha);

    int closureType = closureData.closureType;

    // The tangent frame must be built identically in every branch, because the
    // anisotropic VNDF sample and the anisotropic NDF evaluation are expressed
    // in it. Upstream builds it inside the reflection branch; hoisting it here
    // keeps sampling and evaluation on the same frame.
    vec3 Xa = normalize(X - dot(X, N) * N);
    vec3 Ya = cross(N, Xa);

    // ---- hdClaude: importance sampling -------------------------------------
    // Returns only a direction; see lib/mx_closure_type.glsl for why the
    // density is produced by the evaluation branch instead.
    if (closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 Vt = mx_pt_to_local(V, Xa, Ya, N);
        vec3 Ht = mx_ggx_importance_sample_VNDF(closureData.u.xy, Vt, safeAlpha);
        vec3 H = mx_pt_to_world(Ht, Xa, Ya, N);

        bsdf.sampledL = reflect(-V, H);
        // A perfectly smooth conductor is a delta lobe: next-event estimation
        // must be skipped on it and its MIS weight is one. The threshold is the
        // same alpha clamp the evaluation uses, so the two branches agree about
        // which surfaces are specular.
        bsdf.isDelta = avgAlpha <= M_FLOAT_EPS ? 1.0 : 0.0;
        return;
    }

    // ---- Upstream MaterialX evaluation, unchanged ---------------------------
    if (closureType == CLOSURE_TYPE_REFLECTION)
    {
        vec3 H = normalize(L + V);

        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);

        vec3 Ht = vec3(dot(H, Xa), dot(H, Ya), dot(H, N));

        vec3 F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, fd);

        // Note: NdotL is cancelled out
        bsdf.response = D * F * G * comp * closureData.occlusion * weight / (4.0 * NdotV);

        // ---- hdClaude: density and reconstruction guides --------------------
        // The density is upstream's own VNDF reflection PDF, evaluated on the
        // same microfacet normal and the same tangent frame the response above
        // used. Reusing MaterialX's helper rather than deriving a second GGX
        // density is what keeps sampling and shading in agreement.
        float G1V = mx_ggx_smith_G1(NdotV, avgAlpha);
        bsdf.pdf = dot(N, L) > 0.0
                       ? mx_ggx_VNDF_reflection_PDF(Ht, safeAlpha, G1V, NdotV)
                       : 0.0;
        bsdf.isDelta = avgAlpha <= M_FLOAT_EPS ? 1.0 : 0.0;
        bsdf.guideAlbedo = mx_ggx_dir_albedo(NdotV, avgAlpha, fd) * weight;
        bsdf.guideRoughness = avgAlpha;
    }
}
