#version 460
#include "path_state.glsl"

// Paths that missed geometry: add the environment and retire them.
//
// A dome light's environment map when the scene has one, otherwise a constant
// sky. The stand-in solar disc is added only when the scene has no UsdLux
// lights at all, which is the same condition under which the shade kernel
// samples that sun -- adding it in both places would count it twice.

layout(local_size_x = 64) in;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];
    if (hits.values[path].x >= 0)
    {
        return;   // hit geometry; the shade kernel owns this path
    }

    vec3 direction = pathDirection.values[path];
    vec4 lambda = pathWavelengths.values[path];
    vec4 radiance =
        hdclaude_upsample_emission(hdclaude_environment(direction), lambda,
                                   frame.environmentTemperature,
                                   frame.environmentTemperatureScale);

    // Multiple importance sampling against next-event estimation, which
    // samples this same environment at every shading point. Both strategies
    // reach the environment, so each takes the share the balance heuristic
    // gives it; without that the sky would be counted twice and an enclosed
    // set would render at double brightness.
    //
    // A scatter density of zero means there was no competing strategy -- a
    // camera ray, or a delta closure that next-event estimation cannot sample
    // -- and the environment arrives in full.
    float scatterPdf = pathScatterPdf.values[path];
    if (scatterPdf > 0.0)
    {
        radiance *= hdclaude_mis_weight(scatterPdf,
                                       hdclaude_environment_pdf(direction));
    }

    // The stand-in sun as a disc of finite angular radius, so a mirror can
    // reflect it. Only on the camera ray: for every later bounce the shade
    // kernel has already estimated this sun by next-event estimation, and
    // adding the disc as well would double count it.
    //
    // A path that left a delta closure gets neither, because next-event
    // estimation skips a delta and this test does not know it happened. That
    // is the gap multiple importance sampling closes, and it is why the sun is
    // documented as a fallback rather than a light.
    if (frame.lightCount == 0u && frame.bounce == 0u)
    {
        float cosAngle = dot(direction, normalize(frame.sunDirection.xyz));
        if (cosAngle > cos(max(frame.sunDirection.w, 1.0e-4)))
        {
            radiance += hdclaude_upsample_emission(frame.sunRadiance.rgb, lambda);
        }
    }

    pathRadiance.values[path] += pathThroughput.values[path] * radiance;
    pathThroughput.values[path] = vec4(0.0);
}
