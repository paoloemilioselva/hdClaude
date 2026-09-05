// hdClaude genglsl_pt override of add_bsdf.
//
// Upstream's two lines and its derivation comment are unchanged.
//
// `add` has no authored weight to select a lobe with, so hdClaude selects with
// equal probability and reports the matching equal-weight mixture density.
// Selecting uniformly and reporting a uniform mixture are the same choice made
// twice; they must not be allowed to drift apart, which is why they sit in the
// same function.

#include "lib/mx_closure_type.glsl"
#include "lib/mx_pt_sampling.glsl"

void mx_add_bsdf(ClosureData closureData, BSDF in1, BSDF in2, out BSDF result)
{
    result.response = in1.response + in2.response;

    // We derive the throughput for closure addition as follows:
    //   throughput_1 = 1 - dir_albedo_1
    //   throughput_2 = 1 - dir_albedo_2
    //   throughput_sum = 1 - (dir_albedo_1 + dir_albedo_2)
    //                  = 1 - ((1 - throughput_1) + (1 - throughput_2))
    //                  = throughput_1 + throughput_2 - 1
    result.throughput = max(in1.throughput + in2.throughput - 1.0, 0.0);

    // ---- hdClaude ----------------------------------------------------------
    // Responses add, but densities do not: a density must integrate to one, so
    // the two lobes form an equal-weight mixture rather than a sum.
    result.pdf = 0.5 * (in1.pdf + in2.pdf);
    result.spectrum = in1.spectrum + in2.spectrum;

    // Guides are combined by relative albedo rather than added, because a
    // demodulation albedo above one has no meaning to a reconstruction backend.
    float w1 = mx_pt_luminance_weight(in1.guideAlbedo);
    float w2 = mx_pt_luminance_weight(in2.guideAlbedo);
    float total = w1 + w2;
    float blend = total > 0.0 ? w2 / total : 0.5;
    result.guideAlbedo = min(in1.guideAlbedo + in2.guideAlbedo, vec3(1.0));
    result.guideRoughness = mix(in1.guideRoughness, in2.guideRoughness, blend);

    if (closureData.closureType == CLOSURE_TYPE_PT_SAMPLE)
    {
        float u = closureData.u.z;
        float selectionPdf;
        if (mx_pt_select_lobe(u, 0.5, selectionPdf))
        {
            result.sampledL = in1.sampledL;
            result.isDelta = in1.isDelta;
        }
        else
        {
            result.sampledL = in2.sampledL;
            result.isDelta = in2.isDelta;
        }
    }
    else
    {
        result.isDelta = min(in1.isDelta, in2.isDelta);
    }
}
