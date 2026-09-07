// hdClaude genglsl_pt override of dielectric_bsdf.
//
// The most consequential of the overrides: this closure is the specular lobe of
// every plastic, every coat, and every piece of glass.
//
// MaterialX 1.39.3's REFLECTION branch is verbatim. Two things are hdClaude's:
//
//  1. Importance sampling, choosing between reflection and refraction by
//     Fresnel and sampling the GGX visible-normal distribution for both.
//  2. A real transmission evaluation. Upstream's TRANSMISSION branch calls
//     `mx_surface_transmission`, which looks up a refracted *environment* --
//     a rasteriser's stand-in for transport it cannot perform. A path tracer
//     refracts and continues the path, so that call is replaced by the GGX BTDF
//     and its density.
//
// CLOSURE_TYPE_INDIRECT is dropped for the same reason it is dropped everywhere
// else: it is a prefiltered-environment approximation of the integral this
// renderer computes exactly.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_specular.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_dielectric_bsdf(ClosureData closureData, float weight, vec3 tint, float ior, vec2 roughness, float thinfilm_thickness, float thinfilm_ior, vec3 N, vec3 X, int distribution, int scatter_mode, inout BSDF bsdf)
{
    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }
    if (closureData.closureType != CLOSURE_TYPE_TRANSMISSION &&
        closureData.closureType != CLOSURE_TYPE_PT_SAMPLE && scatter_mode == 1)
    {
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    // Which side of the interface we are on decides the relative IOR, and it is
    // read from the *geometric* relationship between V and N before the normal
    // is flipped forward. Flipping first and asking afterwards always answers
    // "outside", which silently makes exit refractions behave like entries.
    bool entering = dot(N, V) > 0.0;

    // Dispersion, if the material authored it and this lobe transmits.
    //
    // `scatter_mode` 0 is R: a coat, a plastic's specular, anything that only
    // reflects. OpenPBR's dispersion is a property of the *transmitted* medium
    // -- the input is `transmission_dispersion_abbe_number` -- so a reflection
    // only lobe is left alone, and a material that authors dispersion still
    // gets an undispersed coat.
    //
    // The index is taken at the hero wavelength alone, and the shade kernel has
    // already collapsed the packet to that one lane before calling here, so
    // this path carries one wavelength and one refracted direction. Every
    // quantity below -- Fresnel, the refraction direction, the Jacobian, the
    // density -- is then consistently that wavelength's, which is what a
    // per-lane index applied to the direction alone would not be.
    if (scatter_mode != 0)
    {
        ior = hdclaude_dispersed_ior(ior, hdclaude_dispersion_abbe,
                                     hdclaude_wavelengths.x);
    }

    N = mx_forward_facing_normal(N, V);
    float NdotV = clamp(dot(N, V), M_FLOAT_EPS, 1.0);

    FresnelData fd = mx_init_fresnel_dielectric(ior, thinfilm_thickness, thinfilm_ior);
    float F0 = mx_ior_to_f0(ior);

    vec2 safeAlpha = clamp(roughness, M_FLOAT_EPS, 1.0);
    float avgAlpha = mx_average_alpha(safeAlpha);
    vec3 safeTint = max(tint, 0.0);

    // Relative IOR in both conventions, because the refraction helper and the
    // Jacobian are each written to their own standard form.
    float etaI = entering ? 1.0 : ior;   // incident side
    float etaT = entering ? ior : 1.0;   // transmitted side
    float etaRatio = etaI / etaT;        // for mx_pt_refract
    float etaInv = etaT / etaI;          // for the Jacobian

    bool transmissive = scatter_mode != 0;
    // Whether this closure chooses between lobes, and whether it is somebody's
    // layer base. `scatter_mode` is MaterialX's enum: 0 is R, 1 is T, 2 is RT.
    // Only RT chooses; only T is a base.
    bool choosesLobe = scatter_mode == 2;
    bool layerBase = scatter_mode == 1;
    bool smoothSurface = avgAlpha <= M_FLOAT_EPS;

    // The tangent frame is built once so sampling and evaluation share it.
    vec3 Xa = normalize(X - dot(X, N) * N);
    vec3 Ya = cross(N, Xa);

    // Directional albedo drives `throughput`, which is what a `layer` above this
    // closure selects on. Computed before the branch dispatch so that sampling
    // publishes it too -- an early return that skipped it would leave the layer
    // choosing on a stale value.
    vec3 Fv = mx_compute_fresnel(NdotV, fd);
    vec3 compV = mx_ggx_energy_compensation(NdotV, avgAlpha, Fv);
    vec3 dirAlbedoV = mx_ggx_dir_albedo(NdotV, avgAlpha, F0, 1.0) * compV;
    bsdf.throughput = 1.0 - dirAlbedoV * weight;

    // ---- hdClaude: importance sampling -------------------------------------
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 Vt = mx_pt_to_local(V, Xa, Ya, N);
        vec3 Ht = mx_ggx_importance_sample_VNDF(hdclaude_sample_u.xy, Vt, safeAlpha);
        vec3 H = mx_pt_to_world(Ht, Xa, Ya, N);

        // Select reflection or refraction by the Fresnel term at the sampled
        // microfacet, not at the shading normal: on a rough surface those differ
        // substantially at grazing angles, and selecting on the wrong one biases
        // the split between the lobes.
        float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);
        vec3 Fh = mx_compute_fresnel(VdotH, fd);
        // The reflect/refract split is a choice only RT has to make. Asked for
        // one lobe, this closure has no alternative to weigh, and applying the
        // split anyway makes a transmission-only request reflect one sample in
        // twenty that nobody asked for.
        float reflectProbability =
            choosesLobe ? clamp(mx_pt_luminance_weight(Fh), 0.05, 0.95)
                        : (transmissive ? 0.0 : 1.0);

        float u = hdclaude_sample_u.z;
        float selectionPdf;
        vec3 refracted;
        if (mx_pt_select_lobe(u, reflectProbability, selectionPdf) ||
            !mx_pt_refract(V, H, etaRatio, refracted))
        {
            // Reflection, or total internal reflection, which is reflection
            // whatever the caller asked for.
            bsdf.sampledL = reflect(-V, H);
        }
        else
        {
            bsdf.sampledL = normalize(refracted);
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

        vec3 F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);

        bsdf.response = D * F * G * comp * safeTint * closureData.occlusion * weight / (4.0 * NdotV);

        // ---- hdClaude: density and reconstruction guides --------------------
        // Scaled by the same probability the sampler selects reflection with,
        // so that f/pdf is the correct one-sample estimator of the combined
        // reflect/refract lobe rather than of the reflection lobe alone.
        float G1V = mx_ggx_smith_G1(NdotV, avgAlpha);
        float reflectProbability =
            choosesLobe ? clamp(mx_pt_luminance_weight(F), 0.05, 0.95) : 1.0;
        bsdf.pdf = dot(N, L) > 0.0
                       ? mx_ggx_VNDF_reflection_PDF(Ht, safeAlpha, G1V, NdotV) *
                             reflectProbability
                       : 0.0;
        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
        bsdf.guideAlbedo = dirAlbedoV * safeTint * weight;
        bsdf.guideRoughness = avgAlpha;
    }
    else if (closureData.closureType == CLOSURE_TYPE_TRANSMISSION)
    {
        if (!transmissive)
        {
            bsdf.pdf = 0.0;
            return;
        }

        // The GGX BTDF (Walter et al. 2007), replacing upstream's refracted
        // environment lookup. `L` is on the far side of the surface, so the
        // half vector is the refraction half vector rather than normalize(L+V).
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

        // Same side of the microfacet: not a transmission configuration.
        if (VdotH * LdotH > 0.0)
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

        // A transmission-only lobe exists to be layered *under* a reflection
        // one -- that is the only way `open_pbr_surface` uses it -- and
        // MaterialX's layering convention is that a base does not know the top's
        // Fresnel: `layer` supplies it as `top.throughput`, which is `1 - F`.
        // Carrying it here as well applies it twice, and `F + (1 - F)^2` is four
        // per cent short of one at normal incidence. The RT mode keeps it,
        // because there it weighs its own two lobes against each other and
        // nothing above supplies anything.
        vec3 fresnelWeight = layerBase ? vec3(1.0) : (vec3(1.0) - F);
        bsdf.response = fresnelWeight * btdf * safeTint * weight;

        // ---- hdClaude: density -----------------------------------------------
        float G1V = mx_ggx_smith_G1(NdotV, avgAlpha);
        float pdfH = mx_ggx_NDF(Hlocal, safeAlpha) * G1V * abs(VdotH) /
                     max(NdotV, M_FLOAT_EPS);
        float jacobian = mx_pt_refraction_jacobian(VdotH, LdotH, etaInv);
        float refractProbability =
            choosesLobe ? 1.0 - clamp(mx_pt_luminance_weight(F), 0.05, 0.95)
                        : 1.0;
        bsdf.pdf = pdfH * jacobian * refractProbability;

        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
        bsdf.guideAlbedo = safeTint * weight;
        bsdf.guideRoughness = avgAlpha;
    }
}
