// hdClaude genglsl_pt override of multiply_bsdf_float.
//
// MaterialX 1.39.3's body is reproduced verbatim. A scalar or colour multiplier
// scales the response and leaves the direction distribution alone, so the
// density passes through untouched -- scaling it as well would be a double
// count that no furnace test at a single weight would reveal.

#include "lib/mx_closure_type.glsl"

void mx_multiply_bsdf_float(ClosureData closureData, BSDF in1, float in2, out BSDF result)
{
    float weight = clamp(in2, 0.0, 1.0);
    result.response = in1.response * weight;
    result.throughput = in1.throughput * weight;

    // ---- hdClaude ----------------------------------------------------------
    result.pdf = in1.pdf;
    result.sampledL = in1.sampledL;
    result.isDelta = in1.isDelta;
    result.spectrum = in1.spectrum * weight;
    result.guideAlbedo = in1.guideAlbedo * weight;
    result.guideRoughness = in1.guideRoughness;
}
