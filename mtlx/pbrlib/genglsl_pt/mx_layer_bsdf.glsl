// hdClaude genglsl_pt override of layer_bsdf.
//
// Upstream's two lines are unchanged.
//
// Layering is the combinator most easily got wrong. Upstream already carries
// the quantity the correct sampling needs: `top.throughput` is
// `1 - directional_albedo(top)`, so the probability that a path interacts with
// the top layer rather than passing through to the base is
//
//     p_top = 1 - average(top.throughput)
//
// Selecting with that probability, and reporting the matching mixture density,
// makes the layer's sampling consistent with the energy split its *evaluation*
// already performs. Selecting with a fixed probability instead still converges,
// but converges slowly and unevenly across roughness, which reads as "layered
// materials are noisy" rather than as a bug.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_layer_bsdf(ClosureData closureData, BSDF top, BSDF base, out BSDF result)
{
    result.response = top.response + base.response * top.throughput;
    result.throughput = top.throughput * base.throughput;

    // ---- hdClaude ----------------------------------------------------------
    // The top layer's directional albedo, averaged across the colour channels,
    // is the fraction of energy it keeps. Clamped away from the endpoints so
    // that neither lobe can be selected with zero probability while still
    // contributing to the response -- which would be an infinite weight.
    vec3 topAlbedo = clamp(vec3(1.0) - top.throughput, 0.0, 1.0);
    float pTop = clamp((topAlbedo.x + topAlbedo.y + topAlbedo.z) / 3.0, 0.05, 0.95);

    result.pdf = mix(base.pdf, top.pdf, pTop);
    result.spectrum = top.spectrum + base.spectrum * vec4(top.throughput, 1.0);

    // The visible albedo is the top layer's own plus whatever of the base's
    // survives the top layer's transmission -- the same split the response uses.
    result.guideAlbedo = top.guideAlbedo + base.guideAlbedo * top.throughput;
    result.guideRoughness = mix(base.guideRoughness, top.guideRoughness, pTop);

    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        float u = closureData.u.z;
        float selectionPdf;
        if (mx_pt_select_lobe(u, pTop, selectionPdf))
        {
            result.sampledL = top.sampledL;
            result.isDelta = top.isDelta;
        }
        else
        {
            result.sampledL = base.sampledL;
            result.isDelta = base.isDelta;
        }
    }
    else
    {
        result.isDelta = min(top.isDelta, base.isDelta);
    }
}
