// hdClaude genglsl_pt override of layer_bsdf.
//
// MaterialX 1.39.3's body is reproduced verbatim; hdClaude appends the density,
// the direction selection, and the guide combination. 1.39.3 is the version
// inside OpenUSD 26.03, and its throughput semantics differ from 1.39.6's --
// see docs/implementation-notes.md.
//
// Layering is the combinator most easily got wrong. The quantity correct
// sampling needs is already present: a leaf closure sets
// `bsdf.throughput = 1 - directional_albedo`, so the probability that a path
// interacts with the top layer rather than passing through to the base is
//
//     p_top = 1 - average(top.throughput)
//
// read from the *child*, which is why it is unaffected by 1.39.3 combining
// layer throughputs additively where 1.39.6 multiplies them.
//
// Selecting with that probability, and reporting the matching mixture density,
// makes the layer's sampling consistent with the energy split its evaluation
// already performs. A fixed probability still converges, but converges slowly
// and unevenly across roughness, which reads as "layered materials are noisy"
// rather than as a bug.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_layer_bsdf(ClosureData closureData, BSDF top, BSDF base, out BSDF result)
{
    result.response = top.response + base.response * top.throughput;
    result.throughput = top.throughput + base.throughput;

    // ---- hdClaude ----------------------------------------------------------
    // Clamped away from both endpoints so neither lobe can be selected with
    // zero probability while still contributing to the response, which would be
    // an infinite weight.
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
        float u = hdclaude_sample_u.z;
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
