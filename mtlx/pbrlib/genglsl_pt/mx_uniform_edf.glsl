// hdClaude genglsl_pt override of uniform_edf.
//
// MaterialX 1.39.3's body, verbatim. Nothing is added.
//
// This file exists only to keep the include boundary intact. Every pbrlib
// closure file includes `lib/mx_closure_type.glsl`, resolved relative to its
// own directory, so generating one stock closure beside one hdClaude closure
// emits both copies and declares `struct ClosureData` twice. The override set
// is all-or-nothing; see hdclaude_pbrlib_impl.mtlx.
//
// Emission carries no direction to sample, so unlike the BSDFs these need no
// PT_SAMPLE branch and no density: the integrator asks for emission with
// CLOSURE_TYPE_EMISSION and reads the result.

#include "lib/mx_closure_type.glsl"

void mx_uniform_edf(ClosureData closureData, vec3 color, out EDF result)
{
    if (closureData.closureType == CLOSURE_TYPE_EMISSION)
    {
        result = color;
    }
}
