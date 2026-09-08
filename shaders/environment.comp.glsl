#version 460
#include "path_state.glsl"

// Paths that missed geometry: add the environment and retire them.
//
// A dome light's environment map when the scene has one, otherwise a constant
// sky. The stand-in solar disc is added only when the scene has no UsdLux
// lights at all, which is the same condition under which the shade kernel
// samples that sun -- adding it in both places would count it twice.

layout(local_size_x = 64) in;

layout(push_constant) uniform EnvironmentParams {
    /// Which bounce this dispatch is, counted from zero.
    ///
    /// A push constant rather than a field of the frame uniform, and that is what
    /// lets a whole sample be recorded into one command buffer. The uniform is
    /// written by the *host*, so a value that varied per bounce forced a submit
    /// and a full wait between every pair of bounces -- 288 of them per gallery
    /// frame -- and every dispatch in a batched buffer would otherwise have read
    /// whichever value happened to be written last.
    uint bounce;
} params;

void main()
{
    uint slot = gl_GlobalInvocationID.x;
    if (slot >= counters.activeCount)
    {
        return;
    }
    uint path = activeQueue.values[slot];
    int record = hits.values[path].x;
    if (record >= 0)
    {
        return;   // hit geometry; the shade kernel owns this path
    }

    vec3 direction = pathDirection.values[path];
    vec4 lambda = pathWavelengths.values[path];

    // --- A ray that struck an analytic light ---------------------------------
    //
    // The other half of next-event estimation. A light is now an opaque emitter
    // a scattered ray can reach, so the same light arrives by two strategies and
    // the balance heuristic decides the share of each. Without a weight here a
    // rough surface would count every light twice.
    //
    // A camera ray, and a ray leaving a delta closure, carry a scatter density
    // of zero: next-event estimation could not have produced them -- it is
    // skipped entirely on a delta -- so there is no second strategy and the
    // light arrives in full. That is what puts a light's reflection in a mirror
    // and its image in a glass ball, which is the whole point of making it
    // hittable.
    if (record <= -2)
    {
        uint index = uint(-2 - record);
        Light light = lights.values[index];

        vec3 normal;
        float distance = hdclaude_intersect_light(
            light, pathOrigin.values[path], direction, normal);
        if (distance > 0.0)
        {
            vec4 emission = pathThroughput.values[path] *
                            hdclaude_upsample_emission(
                                light.radiance *
                                    hdclaude_light_shaping(light, -direction),
                                lambda, light.colorTemperature,
                                light.temperatureScale);

            float scatterPdf = pathScatterPdf.values[path];
            if (scatterPdf > 0.0)
            {
                float lightPdf =
                    hdclaude_light_hit_pdf(light, distance, normal, direction) /
                    float(hdclaude_emitter_count());
                emission *= hdclaude_mis_weight(scatterPdf, lightPdf);
            }
            pathRadiance.values[path] += emission;
        }
        pathThroughput.values[path] = vec4(0.0);
        return;
    }

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

    // A distant light's disc. It sits at infinity, so the only ray that reaches
    // one is a ray that reached nothing else, which is why it is added here
    // rather than alongside the area lights above.
    //
    // It has to be added *somewhere*, now that next-event estimation weighs the
    // analytic lights: a strategy that is weighted down and never made up by a
    // second one loses that energy outright. So a distant light is hittable for
    // the same reason a rect light is, and by the same cone its sampler uses.
    for (uint i = 0u; i < frame.lightCount; ++i)
    {
        Light distant = lights.values[i];
        if (distant.type != HDCLAUDE_LIGHT_DISTANT)
        {
            continue;
        }
        vec3 axis = normalize(-distant.direction);
        float cosMax = cos(max(distant.angularRadius, 1.0e-4));
        if (dot(direction, axis) <= cosMax)
        {
            continue;
        }

        vec4 emission = hdclaude_upsample_emission(
            distant.radiance, lambda, distant.colorTemperature,
            distant.temperatureScale);
        if (scatterPdf > 0.0)
        {
            // The cone density its sampler returns, times the chance of having
            // chosen this emitter.
            float lightPdf =
                (1.0 / (6.28318530718 * max(1.0e-6, 1.0 - cosMax))) /
                float(hdclaude_emitter_count());
            emission *= hdclaude_mis_weight(scatterPdf, lightPdf);
        }
        radiance += emission;
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
    if (hdclaude_has_stand_in_sun() && params.bounce == 0u)
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
