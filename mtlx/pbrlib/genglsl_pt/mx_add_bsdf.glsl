// hdClaude genglsl_pt override of add_bsdf.
//
// MaterialX 1.39.3's body is reproduced verbatim; hdClaude appends the density,
// the direction selection, and the guide combination. 1.39.3 is the version
// inside OpenUSD 26.03, and its throughput semantics differ from 1.39.6's --
// see docs/implementation-notes.md.
//
// `add` has no authored weight to select a lobe with, so hdClaude selects with
// equal probability and reports the matching equal-weight mixture density.
// Selecting uniformly and reporting a uniform mixture are the same choice made
// twice; they sit in one function so they cannot drift apart.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_add_bsdf(ClosureData closureData, BSDF in1, BSDF in2, out BSDF result)
{
    result.response = in1.response + in2.response;
    result.throughput = in1.throughput + in2.throughput;

    // ---- hdClaude ----------------------------------------------------------
    // Responses add, but densities do not: a density must integrate to one, so
    // the two lobes form an equal-weight mixture rather than a sum.
    result.pdf = 0.5 * (in1.pdf + in2.pdf);
    result.spectrum = in1.spectrum + in2.spectrum;

    // Guides are combined by relative albedo rather than added, because a
    // demodulation albedo above one has no meaning to a reconstruction backend.
    float w1 = mx_pt_luminance_weight(in1.guideDiffuse + in1.guideSpecular);
    float w2 = mx_pt_luminance_weight(in2.guideDiffuse + in2.guideSpecular);
    float total = w1 + w2;
    float blend = total > 0.0 ? w2 / total : 0.5;
    result.guideDiffuse = min(in1.guideDiffuse + in2.guideDiffuse, vec3(1.0));
    result.guideSpecular = min(in1.guideSpecular + in2.guideSpecular, vec3(1.0));
    result.guideNormal = mix(in1.guideNormal, in2.guideNormal, blend);
    result.guideRoughness = mix(in1.guideRoughness, in2.guideRoughness, blend);

    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        float u = mx_pt_selection_random();
        float selectionPdf;
        if (mx_pt_select_lobe(u, 0.5, selectionPdf))
        {
            result.sampledL = in1.sampledL;
            result.isDelta = in1.isDelta;
            hdclaude_carry_medium(result, in1);
        }
        else
        {
            result.sampledL = in2.sampledL;
            result.isDelta = in2.isDelta;
            hdclaude_carry_medium(result, in2);
        }
    }
    else
    {
        result.isDelta = min(in1.isDelta, in2.isDelta);
        hdclaude_select_medium(result, in1, 0.5 * in1.pdf, in2, 0.5 * in2.pdf,
                               mx_pt_selection_random());
    }
}
