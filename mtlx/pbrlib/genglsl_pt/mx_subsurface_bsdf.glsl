// hdClaude genglsl_pt override of subsurface_bsdf.
//
// This is the node whose stock GLSL implementation forced hdClaude to carry its
// own `lib/mx_microfacet_diffuse.glsl`. Upstream's genglsl body calls
// `mx_subsurface_scattering_approx`, which estimates curvature from screen-space
// derivatives -- entirely reasonable in a fragment shader, meaningless in a
// wavefront kernel where neighbouring lanes are unrelated paths. See
// docs/implementation-notes.md.
//
// hdClaude does not approximate subsurface transport; it performs it. But not
// *here*: a bounded spectral random walk happens along paths inside the medium,
// which is the integrator's job, not a closure evaluation at a point. Compare
// MaterialX's own OSL target, which emits a `subsurface_bssrdf` closure and
// leaves the transport to the renderer -- the same division of labour.
//
// So this closure has two jobs:
//
//  1. Publish the medium parameters for the integrator's random walk.
//  2. Provide the *boundary condition*: the cosine-weighted distribution by
//     which a path enters the surface, and its density.
//
// That second part is not an approximation of the transport. It is the entry
// distribution a random-walk BSSRDF genuinely has; what happens after entry is
// decided by the walk, using the parameters published above.
//
// ---- The two mappings, and where they come from ---------------------------
//
// `color` is documented by MaterialX as the diffuse reflectivity and by OpenPBR
// as "the observed reflection color of the subsurface scattering medium". That
// is the light that comes back *out*, not the survival probability of one
// collision, and the two are far apart -- a walk whose collisions each survive
// with probability 0.5 returns about a tenth of what enters it. OpenPBR states
// the inversion between them in closed form, from van de Hulst. Handing `color`
// to the walk as its scattering albedo instead would render every subsurface
// material far darker than it was authored, in a way that looks like a lighting
// problem.
//
// The inversion is *not* applied here. It is steeply nonlinear, and applying a
// nonlinearity to an RGB triple before it becomes a spectrum is how a colour
// stops meaning what it was authored to mean: the walk then returns a spectrum
// whose projection is some other colour, and a magenta material measured a
// seventh short in red. The reflectance is published as it stands and the
// integrator inverts it per wavelength, which is the one arrangement in which
// the light that comes back out *is* `color`.
//
// `radius` is the per-channel mean free path, and OpenPBR's extinction is its
// reciprocal: "the extinction coefficient is simply given by the reciprocal of
// the MFP per channel: mu_t = 1/r (this may need to be regularized in the limit
// r -> 0 to avoid numerical issues)". Both halves matter here, because the
// bubblegum asset authors a radius of (1, 0, 0.068): a zero component is legal,
// and it is the case that has to be regularized.
//
// The regularization is a floor on the *ratio* between channels rather than an
// epsilon on the value, and its size is not a matter of taste. A channel a
// factor R denser than the least dense one arrives at
// `hdclaude_lane_extinction` as a transmittance of `exp(-R)` over the medium's
// own reference distance, and that function clamps the upsampled transmittance
// at 1e-8, so `R = -log(1e-8) = 18.42` is the largest ratio the spectral
// mapping can carry at all. Below it, nothing changes: a medium that dense is in
// the diffusive regime, where van de Hulst's reflectance -- the whole of the
// visible effect -- does not depend on the mean free path. An epsilon on the
// value would instead have made the floor depend on the scene's unit of length.
//
// A radius of zero in *every* channel is not regularized but solved. The light
// leaves where it entered, so the response is exactly the semi-infinite
// reflectance the inversion is written against, which is `color`: the closure
// becomes a Lambertian of that reflectance, with no medium and no walk. That is
// the limit rather than a fallback, and it is why this file has two branches.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_diffuse.glsl"
#include "lib/mx_pt_sampling.glsl"

// The largest ratio between the densest and least dense channel that survives
// the spectral mapping of a coefficient. See the note above.
#define HDCLAUDE_SUBSURFACE_MFP_RATIO 18.42

void mx_subsurface_bsdf(ClosureData closureData, float weight, vec3 color, vec3 radius, float anisotropy, vec3 N, inout BSDF bsdf)
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

    vec3 reflectance = clamp(color, vec3(0.0), vec3(1.0));
    float g = clamp(anisotropy, -0.99, 0.99);
    vec3 mfp = max(radius, vec3(0.0));
    float longest = max(mfp.x, max(mfp.y, mfp.z));

    // Whether there is a medium to walk through at all, or whether the light
    // leaves where it entered.
    bool walks = longest > 0.0;

    if (walks)
    {
        // ---- hdClaude: publish the medium for the integrator's random walk --
        // The integrator reads this from the lobe it selected, when a path
        // enters the surface, and walks until it reaches a boundary.
        mfp = max(mfp, vec3(longest / HDCLAUDE_SUBSURFACE_MFP_RATIO));
        hdclaude_publish_subsurface(bsdf, vec3(1.0) / mfp, reflectance, g);
    }

    // ---- hdClaude: the boundary --------------------------------------------
    //
    // Cosine-weighted about the side the light goes to. With a medium that is
    // *into* the surface; the interface carries no Fresnel and no tint, because
    // it is index-matched -- the node has no index of refraction among its
    // inputs and van de Hulst's relation assumes none -- and the colour is
    // already in the medium, where applying it here as well would count it
    // twice. Without a medium the same distribution points the other way and
    // the tint is the whole answer, because there is no walk to carry it.
    //
    // A cosine lobe and not the undeviated direction, which is what an
    // index-matched interface does to a ray, and this is a decision rather than
    // an oversight. Two reasons, and the first is the one that settles it.
    //
    // An undeviated entry is a delta, and a delta has no solid-angle density to
    // report. Every surface model that uses this node puts it inside a `mix` --
    // `standard_surface` against its diffuse lobe on `subsurface`,
    // `open_pbr_surface` on `subsurface_weight` -- and a combinator must report
    // the density of the mixture. Handing it a one where its sibling reports a
    // real density is not a mixture of anything. The alternative, a very narrow
    // lobe of some invented width, is the sort of constant this renderer does
    // not have.
    //
    // And it is the arrangement in which the authored colour is the colour seen
    // from every direction. Van de Hulst's relation has no angular argument: it
    // relates an albedo to the fraction of entering light that returns, which is
    // what OpenPBR's own energy constraint uses it as. A walk entered along the
    // incident direction reflects that fraction only when averaged over the
    // hemisphere, and less than it head on. Measured on a sphere of a 0.6
    // material in a unit furnace, the undeviated entry reads 0.5577 and the
    // cosine entry 0.6173; at 0.2 they read 0.1684 and 0.2093. Both conserve
    // energy exactly -- the lossless furnace reads 1.0021 and 1.0018 -- so this
    // is where the light goes, not how much of it there is.
    //
    // What that costs is the angular structure of subsurface reflection, which
    // for a medium diffusive enough to look like subsurface scattering is
    // slight, and the two to four per cent above.
    vec3 side = walks ? -N : N;

    // ---- hdClaude: reconstruction guides -----------------------------------
    // Properties of the surface and the view alone, so they are written before
    // the branch dispatch and every pass -- sampling included -- publishes the
    // same values (docs/dlss-integration.md 4).
    bsdf.guideDiffuse = reflectance * weight;
    bsdf.guideSpecular = vec3(0.0);
    bsdf.guideNormal = N;
    bsdf.guideRoughness = 1.0;

    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 X, Y;
        mx_pt_basis(side, X, Y);
        vec3 local = mx_pt_sample_cosine_hemisphere(hdclaude_sample_u.xy);
        bsdf.sampledL = mx_pt_to_world(local, X, Y, side);
        bsdf.isDelta = 0.0;
        return;
    }

    // The direction has to be on the side the walk needs it on, and the
    // integrator has already decided which evaluation type that is from which
    // side `L` left on. A subsurface lobe with a medium answers only the
    // transmission type, and one without it only the reflection type; asking
    // the other gets zero response and zero density, which is what the mixture
    // above needs in order to report a density that describes the sampling.
    int wanted = walks ? CLOSURE_TYPE_TRANSMISSION : CLOSURE_TYPE_REFLECTION;
    if (closureData.closureType != wanted)
    {
        bsdf.pdf = 0.0;
        return;
    }

    float NdotL = dot(side, L);
    if (!(NdotL > 0.0))
    {
        bsdf.pdf = 0.0;
        return;
    }

    // The entry is index-matched, so `f / pdf` is one and everything the
    // material does to the light happens inside. Without a medium it is the
    // Lambertian the r -> 0 limit reduces to, and `f / pdf` is the reflectance.
    vec3 boundary = walks ? vec3(1.0) : reflectance;
    bsdf.response = boundary * weight * NdotL * M_PI_INV *
                    (walks ? 1.0 : closureData.occlusion);

    bsdf.pdf = mx_pt_cosine_hemisphere_pdf(NdotL);
    bsdf.isDelta = 0.0;
}
