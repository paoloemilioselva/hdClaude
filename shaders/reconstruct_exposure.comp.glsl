#version 460
#extension GL_EXT_scalar_block_layout : require

// The frame's exposure value, for a reconstruction backend that asks for one.
//
// DLSS processing HDR needs "the renderer's exposure value for the current
// frame ... the value which when multiplied to the input color values brings
// middle gray to an expected level", supplied as a 1x1 texture (DLSS
// Programming Guide 3.9). Without it the guide's own list of symptoms includes
// the frame "being too dark or too bright", and that is exactly what hdClaude
// was producing: a scene lit only by a small, very bright emitter kept 58% of
// its light through DLAA where a well-exposed one kept 92%
// (docs/dlss-integration.md 5b).
//
// The formula is the guide's, verbatim:
//
//     ExposureValue = MidGray / (AverageLuma * (1 - MidGray))
//
// with MidGray 0.18 -- "the perceptual middle between full reflected brightness
// and full absorption" -- and AverageLuma the average luminance of the entire
// frame.
//
// The average arrives as one partial sum per workgroup of the packing kernel,
// which was already reading every pixel. This kernel finishes the reduction and
// writes the single value. Two stages rather than one invocation summing
// everything: at 4K there are 130,000 partials, and a serial pass over them on
// one lane is milliseconds of an interactive frame for a single float.

layout(local_size_x = 256) in;

layout(set = 0, binding = 0, scalar) readonly buffer LuminancePartials {
    float values[];
} partials;

layout(set = 0, binding = 1, r16f) uniform writeonly image2D exposureImage;

layout(push_constant) uniform Params {
    uint partialCount;
    uint pixelCount;
} params;

shared float sums[256];

void main()
{
    const uint lane = gl_LocalInvocationIndex;

    // Strided, so the whole workgroup shares the walk however many partials
    // there are, and every invocation reaches the barriers below whether or not
    // it found anything to add.
    float total = 0.0;
    for (uint i = lane; i < params.partialCount; i += 256u)
    {
        total += partials.values[i];
    }
    sums[lane] = total;
    barrier();

    for (uint stride = 128u; stride > 0u; stride >>= 1)
    {
        if (lane < stride)
        {
            sums[lane] += sums[lane + stride];
        }
        barrier();
    }

    if (lane != 0u)
    {
        return;
    }

    const float midGray = 0.18;
    float exposure = 1.0;

    if (params.pixelCount > 0u)
    {
        const float averageLuma = sums[0] / float(params.pixelCount);
        // A frame with no light in it has no middle grey, and the formula
        // divides by zero. One is the honest neutral: it scales nothing, which
        // is the only defensible answer about an image that is not there.
        if (averageLuma > 0.0)
        {
            exposure = midGray / (averageLuma * (1.0 - midGray));
        }
    }

    // Clamped because the destination is a half-float, whose largest finite
    // value is 65504. This is the format's limit and not a tuning: an average
    // luminance below about 3e-6 asks for an exposure this texture cannot
    // carry, and an infinity there would tell DLSS nothing at all. The lower
    // bound is the same statement at the other end.
    exposure = clamp(exposure, 1.0e-4, 6.0e4);

    imageStore(exposureImage, ivec2(0, 0), vec4(exposure, 0.0, 0.0, 0.0));
}
