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
    // read from the *geometric* normal before the shading normal is flipped
    // forward. Flipping first and asking afterwards always answers "outside",
    // which silently makes exit refractions behave like entries.
    //
    // The shading normal cannot answer this. On a smooth-shaded mesh it tilts
    // past the horizon near a silhouette, so `dot(N, V) < 0` happens all over
    // the outside of a perfectly opaque object; deciding the Fresnel curve from
    // it puts a rim of total internal reflection around every rounded thing in
    // the scene. The subdivision gallery scene, whose materials have no
    // transmission at all, is where that showed.
    bool entering = hdclaude_entering(N, V);

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

    // Fresnel is a property of the *relative* index, and which side the path is
    // on decides which way round that ratio goes. Handing the absolute index to
    // both sides is what made a path inside the glass see the outside's
    // reflectance curve: one that reaches unity only at grazing, where the true
    // one reaches it at the critical angle and stays there. So an interface that
    // could transmit nothing at all reported four per cent, and the layering
    // above it handed the other ninety-six to a lobe with nowhere to send it.
    //
    // `mx_fresnel_dielectric` already returns exactly 1.0 when
    // `eta^2 + cos^2 - 1 < 0`, which is the critical angle written out. Nothing
    // here needs to detect total internal reflection; it needs to stop hiding it.
    //
    // The normal-incidence reflectance is unchanged, since `((n-1)/(n+1))^2` is
    // the same for `n` and `1/n`. Only the angular curve moves, and only for a
    // path that is genuinely inside the dense medium.
    //
    // Which is not the same as "arriving from behind". A ray that reaches the
    // back face of an opaque object -- through a hole, or because the mesh is a
    // single-sided shell -- arrives from behind and is in the air. The
    // subdivision gallery scene is exactly that, and reading its back faces as
    // glass put a rim of total internal reflection around three opaque objects.
    //
    // Nor can `scatter_mode` separate the two: the reflection half of a split
    // transmissive interface and a coat over an opaque substrate are both
    // reflection-only lobes, and the first is inside glass while the second is
    // not. What separates them is the *path's* history, which only the
    // integrator has, so it publishes it.
    bool insideDenseMedium = !entering && hdclaude_inside_medium > 0.5;
    float relativeIor =
        insideDenseMedium ? 1.0 / max(ior, M_FLOAT_EPS) : ior;

    FresnelData fd = mx_init_fresnel_dielectric(relativeIor, thinfilm_thickness,
                                                thinfilm_ior);
    float F0 = mx_ior_to_f0(relativeIor);

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

    // This albedo is not decoration: `layer` reads it as the *energy split*, and
    // gives the base `1 - albedo` of the light. So it has to agree with the
    // Fresnel the response above is computed from, or the two halves of one
    // interface disagree about how much light there was.
    //
    // `mx_ggx_dir_albedo(NdotV, alpha, F0, 1.0)` interpolates F0 to F90 on a
    // Schlick-shaped curve. From outside that is the right family and it stays.
    // From inside it is the wrong one at every angle: the true curve climbs to
    // one at the critical angle and stays there, where Schlick reaches one only
    // at grazing. Below the critical angle it therefore *understates* the
    // reflectance, the layer hands the surplus to the transmission lobe, and the
    // interface passes on more light than it received.
    //
    // So on the inside the same lobe energy is weighted by the Fresnel actually
    // in force. `mx_ggx_dir_albedo(NdotV, alpha, 1.0, 1.0)` is that lobe energy
    // -- the library's own `Ess` -- and for a smooth surface it is one, which
    // makes this exactly the reflectance and the split exactly right.
    vec3 dirAlbedoV =
        insideDenseMedium
            ? mx_ggx_dir_albedo(NdotV, avgAlpha, 1.0, 1.0) * Fv * compV
            : vec3(mx_ggx_dir_albedo(NdotV, avgAlpha, F0, 1.0)) * compV;

    // And past the critical angle the lobe keeps everything, whatever a fit or
    // an energy term says. Saying so is what makes a `layer` above choose this
    // lobe rather than one that has nowhere to send the light.
    if (Fv.x >= 1.0)
    {
        dirAlbedoV = vec3(1.0);
    }
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
        //
        // Unclamped, for the same reason the `layer` combinator's own selection
        // is: the two conditions coincide. A dielectric's Fresnel is never zero,
        // so the reflection lobe cannot be given no probability while still
        // contributing; and where it is *one*, refraction is impossible and the
        // transmission lobe contributes nothing. The old 0.95 ceiling was the
        // expensive half -- past the critical angle the sampler reflects every
        // time, whatever it says here, so reporting 0.95 divided each of those
        // samples by a density five per cent smaller than the one that produced
        // it, and a path bouncing inside glass paid that on every crossing.
        float reflectProbability =
            choosesLobe ? clamp(mx_pt_luminance_weight(Fh), 0.0, 1.0)
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
            choosesLobe ? clamp(mx_pt_luminance_weight(F), 0.0, 1.0) : 1.0;
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
            choosesLobe ? 1.0 - clamp(mx_pt_luminance_weight(F), 0.0, 1.0)
                        : 1.0;
        bsdf.pdf = pdfH * jacobian * refractProbability;

        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
        bsdf.guideAlbedo = safeTint * weight;
        bsdf.guideRoughness = avgAlpha;
    }
}
