// hdClaude genglsl_pt override of generalized_schlick_bsdf.
//
// Structurally identical to the dielectric override: the same GGX machinery
// with an artistic Schlick Fresnel in place of the physical dielectric one. The
// commentary on sampling, on the transmission evaluation replacing upstream's
// refracted environment lookup, and on why the tangent frame and directional
// albedo are hoisted, is in mx_dielectric_bsdf.glsl and is not repeated here.
//
// MaterialX 1.39.3's REFLECTION branch is verbatim.
//
// One difference worth naming: the relative IOR for refraction is recovered
// from the average F0 through `mx_f0_to_ior`, exactly as upstream does in its
// own transmission branch. An artistic Fresnel does not carry an IOR, so this
// is the only definition of "which way does light bend" available, and using
// anything else would make the refracted direction disagree with the Fresnel
// weighting applied to it.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_specular.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_generalized_schlick_bsdf(ClosureData closureData, float weight, vec3 color0, vec3 color82, vec3 color90, float exponent, vec2 roughness, float thinfilm_thickness, float thinfilm_ior, vec3 N, vec3 X, int distribution, int scatter_mode, inout BSDF bsdf)
{
    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    bool entering = dot(N, V) > 0.0;

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    vec3 safeColor0 = max(color0, 0.0);
    vec3 safeColor82 = max(color82, 0.0);
    vec3 safeColor90 = max(color90, 0.0);
    FresnelData fd = mx_init_fresnel_schlick(safeColor0, safeColor82, safeColor90, exponent, thinfilm_thickness, thinfilm_ior);

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

    // Recovered from the average F0; see the header note.
    float avgF0 = dot(safeColor0, vec3(1.0 / 3.0));
    float ior = mx_f0_to_ior(avgF0);
    float etaI = entering ? 1.0 : ior;
    float etaT = entering ? ior : 1.0;
    float etaRatio = etaI / etaT;
    float etaInv = etaT / etaI;

    bool transmissive = scatter_mode != 0;
    // A lobe is a delta whenever *either* alpha is degenerate, not only when
    // their average is.
    //
    // `mx_average_alpha` is the geometric mean of the *clamped* pair, so an
    // alpha of zero in one axis arrives as the epsilon and the mean lands well
    // above it. That matters because OpenPBR's anisotropy mapping produces
    // exactly that: `alpha_y = (1 - anisotropy) * alpha_x`, so an anisotropy of
    // one -- which its own specification permits and the Playground's bottle
    // authors on its coat -- is alpha_y of zero by definition. Such a
    // distribution is a delta in that axis; next-event estimation cannot
    // evaluate a delta at a single direction, and evaluating it anyway gave
    // that scene radiance of 2.18e25.
    //
    // Tested on the unclamped input, since the clamp is what hides it.
    bool smoothSurface = min(roughness.x, roughness.y) <= M_FLOAT_EPS;

    vec3 Xa = normalize(X - dot(X, N) * N);
    vec3 Ya = cross(N, Xa);

    vec3 Fv = mx_compute_fresnel(NdotV, fd);
    vec3 compV = mx_ggx_energy_compensation(NdotV, avgAlpha, Fv);
    vec3 dirAlbedoV = mx_ggx_dir_albedo(NdotV, avgAlpha, safeColor0, safeColor90) * compV;
    float avgDirAlbedo = dot(dirAlbedoV, vec3(1.0 / 3.0));
    bsdf.throughput = vec3(1.0 - avgDirAlbedo * weight);

    // ---- hdClaude: reconstruction guides -----------------------------------
    // Properties of the surface and the view alone, so they are written before
    // the branch dispatch and every pass -- sampling included -- publishes the
    // same values (docs/dlss-integration.md 4).
    // As the dielectric, except that this lobe's transmission carries its own
    // `1 - F`, so a transmit-only lobe returns what reflection leaves.
    bsdf.guideDiffuse = vec3(0.0);
    bsdf.guideSpecular = (scatter_mode == 0 ? dirAlbedoV : (scatter_mode == 1 ? vec3(1.0) - dirAlbedoV : vec3(1.0))) * weight;
    bsdf.guideNormal = N;
    bsdf.guideRoughness = avgAlpha;

    // A transmit-only lobe has nothing to say about reflection -- but it says
    // so only after publishing its guides, because a guide is a property of the
    // surface and every pass has to agree on it. Returning at the top, where
    // upstream does, left a REFLECTION pass with no guides at all, and a
    // transmit-only lobe is exactly what `open_pbr_surface` layers its glass
    // from.
    if (closureData.closureType != CLOSURE_TYPE_TRANSMISSION &&
        closureData.closureType != CLOSURE_TYPE_PT_SAMPLE && scatter_mode == 1)
    {
        return;
    }

    // ---- hdClaude: importance sampling -------------------------------------
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 Vt = mx_pt_to_local(V, Xa, Ya, N);
        vec3 Ht = mx_ggx_importance_sample_VNDF(hdclaude_sample_u.xy, Vt, safeAlpha);
        vec3 H = mx_pt_to_world(Ht, Xa, Ya, N);

        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);
        vec3 Fh = mx_compute_fresnel(VdotH, fd);
        float reflectProbability =
            transmissive ? clamp(mx_pt_luminance_weight(Fh), 0.05, 0.95) : 1.0;

        float u = mx_pt_selection_random();
        float selectionPdf;
        vec3 refracted;
        if (mx_pt_select_lobe(u, reflectProbability, selectionPdf) ||
            !mx_pt_refract(V, H, etaRatio, refracted))
        {
            // Kept only on the view's side, and a refraction only if it
            // crosses: a lobe's sample that lands where the lobe has no
            // density would be evaluated by the other branch and weighted by a
            // density that never produced it. The same rule, for the same
            // measured reason, as mx_dielectric_bsdf.
            vec3 reflected = reflect(-V, H);
            bsdf.sampledL = dot(reflected, N) * dot(V, N) > 0.0 ? reflected
                                                                 : vec3(0.0);
        }
        else
        {
            vec3 transmitted = normalize(refracted);
            bsdf.sampledL = dot(transmitted, N) * dot(V, N) < 0.0 ? transmitted
                                                                   : vec3(0.0);
        }
        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
        return;
    }

    // ---- MaterialX 1.39.3 evaluation, unchanged -----------------------------
    if (closureData.closureType == CLOSURE_TYPE_REFLECTION)
    {
        vec3 H = normalize(L + V);

        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);

        vec3 Ht = vec3(dot(H, Xa), dot(H, Ya), dot(H, N));

        vec3  F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);

        // Note: NdotL is cancelled out
        bsdf.response = D * F * G * comp * closureData.occlusion * weight / (4.0 * NdotV);

        // ---- hdClaude: density and reconstruction guides --------------------
        float G1V = mx_pt_ggx_smith_G1_anisotropic(
            vec3(dot(V, Xa), dot(V, Ya), NdotV), safeAlpha);
        float reflectProbability =
            transmissive ? clamp(mx_pt_luminance_weight(F), 0.05, 0.95) : 1.0;
        bsdf.pdf = dot(N, L) > 0.0
                       ? mx_ggx_VNDF_reflection_PDF(Ht, safeAlpha, G1V, NdotV) *
                             reflectProbability
                       : 0.0;
        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
    }
    else if (closureData.closureType == CLOSURE_TYPE_TRANSMISSION)
    {
        if (!transmissive)
        {
            bsdf.pdf = 0.0;
            return;
        }

        vec3 Ht3 = -(etaI * V + etaT * L);
        float htLength = length(Ht3);
        if (htLength < M_FLOAT_EPS)
        {
            bsdf.pdf = 0.0;
            return;
        }
        vec3 H = Ht3 / htLength;
        if (dot(H, N) < 0.0)
        {
            H = -H;
        }

        float VdotH = dot(V, H);
        float LdotH = dot(L, H);
        float NdotL = abs(dot(N, L));

        // A microfacet both directions can see, or no transmission: Walter et
        // al.'s chi-plus factors, which MaterialX's G2 does not carry. See the
        // same test in mx_dielectric_bsdf for what omitting it measured.
        if (VdotH * dot(V, N) <= 0.0 || LdotH * dot(L, N) <= 0.0)
        {
            bsdf.pdf = 0.0;
            return;
        }

        vec3 Hlocal = vec3(dot(H, Xa), dot(H, Ya), dot(H, N));
        vec3 F = mx_compute_fresnel(abs(VdotH), fd);
        float D = mx_ggx_NDF(Hlocal, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        float denom = etaI * VdotH + etaT * LdotH;
        denom = denom * denom;
        float btdf = denom > 0.0
                         ? (abs(VdotH) * abs(LdotH) * etaT * etaT * D * G) /
                               (NdotV * denom)
                         : 0.0;

        bsdf.response = (vec3(1.0) - F) * btdf * weight;

        float G1V = mx_pt_ggx_smith_G1_anisotropic(
            vec3(dot(V, Xa), dot(V, Ya), NdotV), safeAlpha);
        float pdfH = mx_ggx_NDF(Hlocal, safeAlpha) * G1V * abs(VdotH) /
                     max(NdotV, M_FLOAT_EPS);
        float jacobian = mx_pt_refraction_jacobian(VdotH, LdotH, etaInv);
        float refractProbability =
            1.0 - clamp(mx_pt_luminance_weight(F), 0.05, 0.95);
        bsdf.pdf = pdfH * jacobian * refractProbability;

        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
    }
}
