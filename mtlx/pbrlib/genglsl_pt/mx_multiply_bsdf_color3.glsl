// hdClaude genglsl_pt override of multiply_bsdf_color3.
//
// MaterialX 1.39.3's body is reproduced verbatim. A scalar or colour multiplier
// scales the response and leaves the direction distribution alone, so the
// density passes through untouched -- scaling it as well would be a double
// count that no furnace test at a single weight would reveal.

#include "lib/mx_closure_type.glsl"

void mx_multiply_bsdf_color3(ClosureData closureData, BSDF in1, vec3 in2, out BSDF result)
{
    vec3 tint = clamp(in2, 0.0, 1.0);
    result.response = in1.response * tint;
    result.throughput = in1.throughput * tint;

    // ---- hdClaude ----------------------------------------------------------
    result.pdf = in1.pdf;
    result.sampledL = in1.sampledL;
    result.isDelta = in1.isDelta;
    result.spectrum = in1.spectrum * vec4(tint, 1.0);
    result.guideDiffuse = in1.guideDiffuse * tint;
    result.guideSpecular = in1.guideSpecular * tint;
    result.guideNormal = in1.guideNormal;
    result.guideRoughness = in1.guideRoughness;

    // A tint scales what comes back; it does not change which interior a
    // path enters, so the medium passes through with the direction.
    hdclaude_carry_medium(result, in1);
}
