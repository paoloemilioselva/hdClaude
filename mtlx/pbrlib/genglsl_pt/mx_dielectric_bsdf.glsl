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
    //
    // A thin-walled sheet has no inside at all. OpenPBR has the surface "always
    // flipped so that incident rays enter top-down", so it sees the authored
    // index from both sides.
    bool thinSheet = hdclaude_thin_walled > 0.5;
    bool insideDenseMedium =
        !thinSheet && !entering && hdclaude_inside_medium > 0.5;
    float relativeIor =
        insideDenseMedium ? 1.0 / max(ior, M_FLOAT_EPS) : ior;

    FresnelData fd = mx_init_fresnel_dielectric(relativeIor, thinfilm_thickness,
                                                thinfilm_ior);
    float F0 = mx_ior_to_f0(relativeIor);

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

    // ---- hdClaude: reconstruction guides -----------------------------------
    // Properties of the surface and the view alone, so they are written before
    // the branch dispatch and every pass -- sampling included -- publishes the
    // same values (docs/dlss-integration.md 4).
    // Transmission counts as specular: rough or smooth, what comes through
    // glass depends on the view. A reflect-only lobe keeps its directional
    // albedo; one that transmits returns reflected and refracted light
    // together, which is the tint.
    bsdf.guideDiffuse = vec3(0.0);
    bsdf.guideSpecular = (transmissive ? safeTint : dirAlbedoV * safeTint) * weight;
    bsdf.guideNormal = N;
    bsdf.guideRoughness = avgAlpha;

    // ---- hdClaude: a thin-walled sheet -------------------------------------
    //
    // OpenPBR's thin-walled mode treats the surface as "a 2d sheet with no
    // interior" whose translucent base "reduces to a thin sheet of dielectric",
    // and notes that the reflection "will also technically be modified due to
    // the internal bounces in the sheet". MaterialX 1.39.3's graph does neither:
    // it hands this closure a refracting transmission lobe whatever the flag
    // says. So a lobe that transmits, on a material the integrator has marked
    // thin-walled, is shaded as the sheet here.
    //
    // A sheet of two parallel faces bends nothing, so transmission continues
    // undeviated. Summing the bounces between its faces gives, for a lossless
    // sheet with single-face reflectance F,
    //
    //     reflected    2F / (1 + F)        transmitted    (1 - F) / (1 + F)
    //
    // which sum to one. Roughness shapes the reflection as it always does and is
    // ignored on the transmitted side: the specification gives no closed form
    // for a rough sheet's transmission, and the decision taken in its absence is
    // a delta straight through (docs/roadmap.md, decision log, 2026-09-15).
    //
    // The two modes carry the sheet differently, and the difference is where the
    // top face's reflection already lives. RT is the whole sheet on its own. T is
    // the base `open_pbr_surface` layers under a reflection-only lobe, and that
    // lobe has already reflected F and handed on the rest; of what reaches the
    // base, the sheet's further reflection is `F / (1 + F)` and its transmission
    // `1 / (1 + F)`, which sum to one. Carrying the internal bounces in the base
    // rather than in the reflection-only lobe keeps a coat -- another
    // reflection-only lobe, indistinguishable at runtime -- a single interface.
    if (thinSheet && transmissive)
    {
        // Everything that reaches the sheet is reflected or transmitted.
        bsdf.throughput = vec3(1.0 - weight);

        vec3 sheetReflect = layerBase ? Fv / (1.0 + Fv) : 2.0 * Fv / (1.0 + Fv);
        vec3 sheetTransmit = layerBase ? 1.0 / (1.0 + Fv) : (1.0 - Fv) / (1.0 + Fv);
        float reflectWeight = mx_pt_luminance_weight(sheetReflect);
        float transmitWeight = mx_pt_luminance_weight(sheetTransmit);
        float reflectProbability =
            reflectWeight + transmitWeight > 0.0
                ? reflectWeight / (reflectWeight + transmitWeight)
                : 0.0;

        if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
        {
            float u = mx_pt_selection_random();
            if (u < reflectProbability)
            {
                vec3 Vt = mx_pt_to_local(V, Xa, Ya, N);
                vec3 Ht = mx_ggx_importance_sample_VNDF(hdclaude_sample_u.xy, Vt,
                                                        safeAlpha);
                vec3 H = mx_pt_to_world(Ht, Xa, Ya, N);
                vec3 reflected = reflect(-V, H);
                bsdf.sampledL = dot(reflected, N) > 0.0 ? reflected : vec3(0.0);
                bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
            }
            else
            {
                bsdf.sampledL = -V;
                bsdf.isDelta = 1.0;
            }
            return;
        }

        if (closureData.closureType == CLOSURE_TYPE_REFLECTION)
        {
            vec3 H = normalize(L + V);
            float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);
            float VdotH = clamp(dot(V, H), M_FLOAT_EPS, 1.0);
            vec3 Ht = vec3(dot(H, Xa), dot(H, Ya), dot(H, N));

            // The sheet's reflectance at the microfacet, as the single face's
            // Fresnel is taken at the microfacet everywhere else.
            vec3 F = mx_compute_fresnel(VdotH, fd);
            vec3 sheetF = layerBase ? F / (1.0 + F) : 2.0 * F / (1.0 + F);
            float D = mx_ggx_NDF(Ht, safeAlpha);
            float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);
            vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);
            bsdf.response = D * sheetF * G * comp * safeTint *
                            closureData.occlusion * weight / (4.0 * NdotV);

            float G1V = mx_pt_ggx_smith_G1_anisotropic(
                vec3(dot(V, Xa), dot(V, Ya), NdotV), safeAlpha);
            bsdf.pdf = dot(N, L) > 0.0
                           ? mx_ggx_VNDF_reflection_PDF(Ht, safeAlpha, G1V, NdotV) *
                                 reflectProbability
                           : 0.0;
            bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
            return;
        }

        if (closureData.closureType == CLOSURE_TYPE_TRANSMISSION)
        {
            // A delta straight through, which has no finite value at a
            // direction. It is answered the way a smooth microfacet lobe
            // answers at the direction it sampled: a response and a density
            // that are both very large and whose ratio is the estimate, so a
            // combinator mixing it with a sibling's finite density is dominated
            // by the delta, as it is for a mirror at the floored roughness.
            // Next-event estimation never asks, since the lobe is a delta.
            const float kDeltaDensity = 1.0e7;
            bool straightThrough = dot(L, -V) > 1.0 - 1.0e-5;
            bsdf.response = straightThrough
                                ? sheetTransmit * safeTint * weight * kDeltaDensity
                                : vec3(0.0);
            bsdf.pdf = straightThrough
                           ? (1.0 - reflectProbability) * kDeltaDensity
                           : 0.0;
            bsdf.isDelta = 1.0;
            return;
        }
        return;
    }

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

        float u = mx_pt_selection_random();
        float selectionPdf;
        vec3 refracted;
        const bool choseReflection =
            mx_pt_select_lobe(u, reflectProbability, selectionPdf);
        const bool refracts =
            !choseReflection && mx_pt_refract(V, H, etaRatio, refracted);

        // Total internal reflection at a microfacet, in a lobe that does not
        // own reflection.
        //
        // A transmission-only lobe is the base of a `layer` whose top is the
        // reflection half of the same interface, and that top already reflects
        // every microfacet -- including the ones past the critical angle, where
        // its Fresnel is exactly one. So the reflection this lobe would fall
        // back to is not its to make: it is already counted above, and offering
        // a direction for it makes the layer report a density that does not
        // describe the sampling. `mx_layer_bsdf` mixes the base's density at
        // `1 - p_top` and this lobe answers zero when asked about a reflection,
        // so every such sample was weighed by the top's density alone while
        // both halves had produced it. The estimator then divides by less than
        // it should, once per crossing.
        //
        // It is invisible on a smooth surface -- the microfacet is the surface,
        // so the macro Fresnel is already one and the layer gives this lobe
        // nothing -- and invisible on a flat one, which a path crosses twice.
        // A rough sphere is where it shows: `layer(R, T)` at alpha 0.3 read
        // 1.17 at the centre and 1.97 to 2.34 where the interior angles are
        // steepest, against 1.00 for the same layer smooth.
        //
        // Returning no direction is the consistent answer, not a lost path: the
        // transmission response is zero in exactly these configurations, so
        // these samples carry nothing and cost variance rather than energy. The
        // reflection is still sampled, by the lobe that owns it.
        if (!choseReflection && !refracts && layerBase)
        {
            bsdf.sampledL = vec3(0.0);
            bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
            return;
        }

        if (choseReflection || !refracts)
        {
            // Reflection, or total internal reflection, which is reflection
            // whatever the caller asked for.
            //
            // Kept only if it leaves on the view's side. A microfacet tilted
            // far enough reflects below the surface, where this lobe has no
            // density; the caller would then ask the *transmission* branch
            // about it and weight a reflection sample by a density that never
            // produced it. That was measured: at 34 degrees, alpha 0.3, one
            // sample in 150 went below the horizon this way and the chi-squared
            // test over the sphere failed on exactly those cells. Such a sample
            // is discarded, as pbrt-v4's DielectricBxDF discards it.
            vec3 reflected = reflect(-V, H);
            bsdf.sampledL = dot(reflected, N) * dot(V, N) > 0.0 ? reflected
                                                                 : vec3(0.0);
        }
        else
        {
            // And a refraction only if it crosses.
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

        vec3 F = mx_compute_fresnel(VdotH, fd);
        float D = mx_ggx_NDF(Ht, safeAlpha);
        float G = mx_ggx_smith_G2(NdotL, NdotV, avgAlpha);

        vec3 comp = mx_ggx_energy_compensation(NdotV, avgAlpha, F);

        bsdf.response = D * F * G * comp * safeTint * closureData.occlusion * weight / (4.0 * NdotV);

        // ---- hdClaude: density and reconstruction guides --------------------
        // Scaled by the same probability the sampler selects reflection with,
        // so that f/pdf is the correct one-sample estimator of the combined
        // reflect/refract lobe rather than of the reflection lobe alone.
        float G1V = mx_pt_ggx_smith_G1_anisotropic(
            vec3(dot(V, Xa), dot(V, Ya), NdotV), safeAlpha);
        float reflectProbability =
            choosesLobe ? clamp(mx_pt_luminance_weight(F), 0.0, 1.0) : 1.0;
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

        // A microfacet both directions can see, or no transmission at all.
        //
        // The half vector was turned to face the macro-surface's upper side,
        // and that says nothing about whether it faces *the view*. For a
        // direction far enough off the refracted lobe it does not: the
        // microfacet that would bend V into L points away from V, no ray can
        // arrive at it, and Walter et al.'s BTDF is zero there through the
        // chi-plus factors on (v.m)/(v.n) and (l.m)/(l.n). MaterialX's G2 has
        // no such factor, so without this test the lobe answered with a
        // response and a density for impossible configurations -- the density
        // expected ten thousand samples between 105 and 125 degrees that the
        // sampler, which only draws visible normals, never produced. pbrt-v4
        // rejects the same back-facing microfacets in DielectricBxDF::f and
        // ::PDF. The old test, that V and L lie on opposite sides of H, is
        // implied by this one and missed exactly that case.
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

        // What fraction of the light reaching this lobe crosses the interface.
        //
        // In RT mode the closure weighs its own two lobes, so it is `1 - F`
        // taken at the *microfacet* that bends V into L -- the same microfacet
        // the BTDF above is evaluated on, and the same one the reflection half
        // takes its `F` from. That is Walter et al.'s split and it is exact.
        //
        // A transmission-only lobe is layered *under* a reflection one -- the
        // only way `open_pbr_surface` uses it -- and MaterialX's layering
        // convention is that a base does not know the top's Fresnel: `layer`
        // supplies it as `top.throughput`, which is `1 - A` for the top's
        // *directional albedo* A at the macro-surface normal. Carrying `1 - F`
        // here as well would apply a Fresnel twice, and `F + (1 - F)^2` is four
        // per cent short of one at normal incidence.
        //
        // But a macro-surface average is not the per-microfacet split, and on a
        // rough interface the two are nothing alike. Inside glass, most
        // microfacets a steep ray meets are past the critical angle, where
        // `F(V.H)` is exactly one and nothing crosses at all, while the macro
        // average stays far below one and hands the base light that cannot
        // physically pass. It compounds over every crossing, so it needs a
        // closed shape and a deep path limit to see: a sphere of
        // `layer(R, T)` at alpha 0.3 reads 1.17 at the centre and 1.97 to 2.34
        // off-centre, where the interior angles are steepest, against 1.00 for
        // the same layer smooth. The same sphere in RT mode -- the same lobes,
        // split per microfacet -- reads 0.88 and 0.85.
        //
        // So the split is taken at the microfacet in both modes, and the layer's
        // macro factor is divided back out, leaving exactly `(1 - F(V.H))`
        // once. `A` is this closure's own directional albedo, computed from the
        // same index and roughness the lobe above it reads, which is what
        // `open_pbr_surface` wires: one interface, split into two nodes. Its
        // reflection half is left at `weight` one there -- `specular_weight`
        // reaches it as a modulated index, not as a lobe weight -- so the
        // factor the layer applies is exactly `1 - A`.
        //
        // The floor is what makes the division safe. As `A` approaches one the
        // top keeps everything and the layer multiplies this lobe by nothing,
        // so the product is zero whatever this returns; the floor only stops it
        // being zero times an infinity.
        vec3 fresnelWeight;
        if (layerBase)
        {
            vec3 layerFactor = max(vec3(1.0) - dirAlbedoV, vec3(0.02));
            fresnelWeight = (vec3(1.0) - F) / layerFactor;
        }
        else
        {
            fresnelWeight = vec3(1.0) - F;
        }
        bsdf.response = fresnelWeight * btdf * safeTint * weight;

        // ---- hdClaude: density -----------------------------------------------
        float G1V = mx_pt_ggx_smith_G1_anisotropic(
            vec3(dot(V, Xa), dot(V, Ya), NdotV), safeAlpha);
        float pdfH = mx_ggx_NDF(Hlocal, safeAlpha) * G1V * abs(VdotH) /
                     max(NdotV, M_FLOAT_EPS);
        float jacobian = mx_pt_refraction_jacobian(VdotH, LdotH, etaInv);
        float refractProbability =
            choosesLobe ? 1.0 - clamp(mx_pt_luminance_weight(F), 0.0, 1.0)
                        : 1.0;
        bsdf.pdf = pdfH * jacobian * refractProbability;

        bsdf.isDelta = smoothSurface ? 1.0 : 0.0;
    }
}
