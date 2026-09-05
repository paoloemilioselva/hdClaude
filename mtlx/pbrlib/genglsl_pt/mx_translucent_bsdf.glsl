// hdClaude genglsl_pt override of translucent_bsdf.
//
// MaterialX 1.39.3's CLOSURE_TYPE_REFLECTION branch, verbatim, plus the density
// and the reconstruction guides. CLOSURE_TYPE_INDIRECT is dropped.
//
// This is diffuse *transmission*: upstream inverts the normal because the light
// arrives from the far side of the surface. Sampling has to follow, so the
// cosine hemisphere is built around the inverted normal and the sampled
// direction goes into the surface rather than away from it. Sampling the front
// hemisphere here would produce a lobe that evaluates to zero everywhere it can
// be sampled, which reads as a black material rather than as a bug.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_translucent_bsdf(ClosureData closureData, float weight, vec3 color, vec3 N, inout BSDF bsdf)
{
    bsdf.throughput = vec3(0.0);

    if (weight < M_FLOAT_EPS)
    {
        bsdf.pdf = 0.0;
        return;
    }

    vec3 V = closureData.V;
    vec3 L = closureData.L;

    // Invert normal since we're transmitting light from the other side
    N = -N;

    // ---- hdClaude: importance sampling -------------------------------------
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
        float NdotL = clamp(dot(N, L), 0.0, 1.0);
        bsdf.response = color * weight * NdotL * M_PI_INV;

        // ---- hdClaude: density and reconstruction guides --------------------
        // Measured against the inverted normal, matching the sample above.
        bsdf.pdf = NdotL > 0.0 ? mx_pt_cosine_hemisphere_pdf(NdotL) : 0.0;
        bsdf.isDelta = 0.0;
        bsdf.guideAlbedo = color * weight;
        bsdf.guideRoughness = 1.0;
    }
}
