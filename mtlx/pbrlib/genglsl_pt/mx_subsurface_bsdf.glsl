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

#include "lib/mx_closure_type.glsl"
#include "lib/mx_microfacet_diffuse.glsl"
#include "lib/mx_pt_sampling.glsl"

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
    float occlusion = closureData.occlusion;

    N = mx_forward_facing_normal(N, V);

    // ---- hdClaude: publish the medium for the integrator's random walk ------
    // `radius` is the per-channel mean free path; `anisotropy` is the
    // Henyey-Greenstein g. The integrator reads these when a path enters the
    // surface and walks until it exits.
    hdclaude_subsurface_albedo = color * weight;
    hdclaude_subsurface_radius = max(radius, vec3(0.0));
    hdclaude_subsurface_anisotropy = clamp(anisotropy, -0.99, 0.99);
    hdclaude_subsurface_present = 1.0;

    // ---- hdClaude: the entry distribution ----------------------------------
    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        vec3 X, Y;
        mx_pt_basis(N, X, Y);
        vec3 local = mx_pt_sample_cosine_hemisphere(hdclaude_sample_u.xy);
        bsdf.sampledL = mx_pt_to_world(local, X, Y, N);
        bsdf.isDelta = 0.0;
        return;
    }

    if (closureData.closureType == CLOSURE_TYPE_REFLECTION)
    {
        float NdotL = clamp(dot(N, L), M_FLOAT_EPS, 1.0);

        // Lambertian boundary. The albedo here is the *single-scattering
        // albedo* handed to the walk, not a diffuse approximation of the
        // multiple scattering the walk produces.
        bsdf.response = color * occlusion * weight * NdotL * M_PI_INV;

        bsdf.pdf = dot(N, L) > 0.0 ? mx_pt_cosine_hemisphere_pdf(dot(N, L)) : 0.0;
        bsdf.isDelta = 0.0;
        bsdf.guideAlbedo = color * weight;
        bsdf.guideRoughness = 1.0;
    }
}
