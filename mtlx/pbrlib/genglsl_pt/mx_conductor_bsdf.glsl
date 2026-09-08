// hdClaude genglsl_pt override of conductor_bsdf.
//
// The CLOSURE_TYPE_REFLECTION branch is MaterialX 1.39.3's body, byte for
// byte, plus added lines for the density and the reconstruction guides. Only
// the PT_SAMPLE branch and those lines are hdClaude's.
//
// Written against **1.39.3** -- the version inside OpenUSD 26.03, which is what
// hdClaude links and generates with. 1.39.6 changed this node's signature
// (adding `retroreflective`) and its energy-compensation argument, so a body
// copied from the newer upstream would not match the call this generator emits.
//
// The GGX sampling is upstream's `mx_ggx_importance_sample_VNDF`, used
// verbatim. A second GGX implementation that disagreed with MaterialX's own
// would be a fidelity bug no test of hdClaude against itself could find.
//
// CLOSURE_TYPE_INDIRECT is dropped: it exists upstream to look up a prefiltered
// environment, which is a rasteriser's approximation of the integral a path
// tracer computes exactly. Leaving it in would give the generated code a
// second, disagreeing answer for the same quantity.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_specular.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_conductor_bsdf(ClosureData closureData, float weight, vec3 ior_n, vec3 ior_k, vec2 roughness, float thinfilm_thickness, float thinfilm_ior, vec3 N, vec3 X, int distribution, inout BSDF bsdf)
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

    FresnelData fd = mx_init_fresnel_conductor(ior_n, ior_k, thinfilm_thickness, thinfilm_ior);

    // The floor is 1e-4, not the 1e-8 epsilon, and the difference is a
    // representability limit rather than a taste in roughness.
    //
    // The GGX density carries `(h.y / alpha_y)^2`, so as alpha_y falls the value
    // becomes arbitrarily sensitive to the last bits of the half vector -- and
    // the sampler and the evaluator arrive at that vector by different routes,
    // one from the VNDF and one from `normalize(L + V)`. At 1e-8 they disagree
    // by many orders of magnitude, `f / pdf` stops cancelling, and the estimator
    // returns whatever the rounding happened to be. The OpenPBR Playground's
    // bottle reached 2.18e25 that way: its coat authors `roughness 0.33` with
    // `anisotropy 1`, and OpenPBR's own mapping is `alpha_y = (1 - anisotropy) *
    // alpha_x`, so alpha_y is exactly zero by specification.
    //
    // 1e-4 is chosen from float32 rather than from appearance: `h.y` carries an
    // absolute error near 1e-7, so the ratio stays of order one for any alpha
    // above about 1e-6, and this leaves two decades of margin. A lobe that
    // narrow is far below what any sampling here resolves -- alpha 1e-4 is a
    // perceptual roughness of 0.01 -- and below it the ratio cannot be evaluated
    // at all, which is the honest reason for a floor. Whether the lobe counts as
    // a delta is decided separately, from the *unclamped* input, so this does not
    // make a mirror stop being one.
    vec2 safeAlpha = clamp(roughness, kHdclaudeMinAlpha, 1.0);
    float avgAlpha = mx_average_alpha(safeAlpha);

    // The tangent frame is built once, outside the branches, because the
    // anisotropic VNDF sample and the anisotropic NDF evaluation must be
    // expressed in the same frame. Upstream builds it inside the reflection
    // branch, where sampling cannot reach it.
    vec3 Xa = normalize(X - dot(X, N) * N);
    vec3 Ya = cross(N, Xa);

    // ---- hdClaude: importance sampling -------------------------------------
    // Returns only a direction. The density comes from the evaluation branch,
    // for the reason in lib/mx_closure_type.glsl.
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 Vt = mx_pt_to_local(V, Xa, Ya, N);
        vec3 Ht = mx_ggx_importance_sample_VNDF(hdclaude_sample_u.xy, Vt, safeAlpha);
        vec3 H = mx_pt_to_world(Ht, Xa, Ya, N);

        bsdf.sampledL = reflect(-V, H);
        // A perfectly smooth conductor is a delta lobe: next-event estimation
        // must be skipped and its MIS weight is one. So is one whose alpha is
        // degenerate in *either* axis, which the geometric mean of the clamped
        // pair hides -- see the note in mx_dielectric_bsdf.
        bsdf.isDelta = min(roughness.x, roughness.y) <= M_FLOAT_EPS ? 1.0 : 0.0;
        return;
    }

    // ---- MaterialX 1.39.3 evaluation, unchanged -----------------------------
    if (closureData.closureType == CLOSURE_TYPE_REFLECTION)
    {
        vec3 H = normalize(L + V);

        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);

        vec3 Ht = vec3(dot(H, Xa), dot(H, Ya), dot(H, N));

        vec3 F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);

        // Note: NdotL is cancelled out
        bsdf.response = D * F * G * comp * closureData.occlusion * weight / (4.0 * NdotV);

        // ---- hdClaude: density and reconstruction guides --------------------
        // The density is evaluated on the same microfacet normal and the same
        // tangent frame the response above used, from MaterialX's own NDF and
        // shadowing term. That is what keeps sampling and shading in agreement.
        float G1V = mx_ggx_smith_G1(NdotV, avgAlpha);
        bsdf.pdf = dot(N, L) > 0.0
                       ? mx_ggx_VNDF_reflection_PDF(Ht, safeAlpha, G1V, NdotV)
                       : 0.0;
        bsdf.isDelta = min(roughness.x, roughness.y) <= M_FLOAT_EPS ? 1.0 : 0.0;

        // The closure reports its own albedo. No surface-model name is
        // consulted, which is what lets reconstruction guides work for an
        // arbitrary authored nodegraph (docs/dlss-integration.md 3).
        vec3 Fv = mx_compute_fresnel(NdotV, fd);
        bsdf.guideAlbedo = mx_ggx_dir_albedo(NdotV, avgAlpha, Fv, vec3(1.0)) * weight;
        bsdf.guideRoughness = avgAlpha;
    }
}
