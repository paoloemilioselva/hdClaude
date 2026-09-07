#version 460
#include "path_state.glsl"

// Camera rays, one per pixel, and the initial path state.
//
// Every slot starts active: compaction happens after the first bounce, so this
// kernel writes the queue densely rather than through an atomic.

layout(local_size_x = 8, local_size_y = 8) in;

void main()
{
    uvec2 pixel = gl_GlobalInvocationID.xy;
    if (pixel.x >= frame.resolution.x || pixel.y >= frame.resolution.y)
    {
        return;
    }
    uint index = pixel.y * frame.resolution.x + pixel.x;

    uint rng = hdclaude_seed(index, frame.sampleIndex, 0u);

    // Subpixel jitter. Uniform within the pixel: a reconstruction filter is a
    // film concern and is not folded into the sampling here.
    vec2 jitter = vec2(hdclaude_random(rng), hdclaude_random(rng));
    vec2 uv = (vec2(pixel) + jitter) / vec2(frame.resolution);

    // Right-handed camera looking down -Z, matching USD's convention, so a
    // camera transform arriving from Hydra needs no handedness fix-up.
    //
    // Row 0 of the film is the *bottom* of the image, which is Hydra's render
    // buffer convention -- hdEmbree builds its NDC the same way. Negating y
    // here instead would put row 0 at the top and hand every Hydra host an
    // upside-down image, which is what it did until this line was corrected.
    vec2 ndc = uv * 2.0 - 1.0;
    vec3 directionCamera = normalize(vec3(ndc.x * frame.tanHalfFov * frame.aspect,
                                          ndc.y * frame.tanHalfFov,
                                          -1.0));

    vec3 origin = (frame.cameraToWorld * vec4(0.0, 0.0, 0.0, 1.0)).xyz;
    vec3 direction = normalize((frame.cameraToWorld * vec4(directionCamera, 0.0)).xyz);

    pathOrigin.values[index] = origin;
    pathDirection.values[index] = direction;
    // The hero packet, drawn once and held for the life of the path. Its own
    // random number, taken before the pixel's, so the wavelength sequence is
    // decorrelated from the lens and light sequences rather than sharing their
    // stratification.
    pathWavelengths.values[index] = hdclaude_sample_hero(hdclaude_random(rng));
    pathThroughput.values[index] = vec4(1.0);
    pathRadiance.values[index] = vec4(0.0);
    pathPixel.values[index] = index;
    pathRng.values[index] = rng;
    // No scattering produced this ray, so a miss takes the environment in
    // full rather than a weighted share of it.
    pathScatterPdf.values[index] = 0.0;
    // A camera ray starts in vacuum.
    pathMedium.values[2u * index + 0u] = vec4(0.0);
    pathMedium.values[2u * index + 1u] = vec4(0.0);
    activeQueue.values[index] = index;

    if (index == 0u)
    {
        counters.activeCount = frame.resolution.x * frame.resolution.y;
        counters.nextActiveCount = 0u;
        counters.shadowCount = 0u;
    }
}
