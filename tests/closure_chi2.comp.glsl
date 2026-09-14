// Distribution and furnace harness for a generated MaterialX material.
//
// Appended to a generated material module, like closure_validation.comp.glsl,
// and answering the two questions that harness cannot put exactly.
//
// Chi-squared: whether the directions a closure *samples* are distributed as the
// density it *reports*. The old furnace and density integral are both satisfied
// by a sampler and a density that agree on totals and disagree about where the
// mass is; a test over cells of the sphere is not (docs/materialx-codegen.md 8,
// item 3).
//
// Furnace: whether the importance-sampled estimate of the directional albedo is
// the albedo -- the integral of the response over the sphere, which the same
// pass evaluates by quadrature (item 2). The two agree only if the weights are
// right *and* nothing the closure responds to lies outside what the renderer
// keeps, which is the half chi-squared cannot see: a lobe that discards
// directions it responds in has a sampler and density in perfect agreement and
// loses the light anyway.
//
// No accumulation happens here. Each invocation writes only its own slots, so
// there is nothing to race, nothing to overflow and nothing quantised, and the
// host does the binning and the statistics in double precision. The older
// harness accumulates in 1/512 fixed point, which truncates every sample and is
// a bias of about a tenth of a per cent: too close to a 0.5% gate.
//
// mode 0  sample. Stride 5: values[5i..5i+2] = L; values[5i+3] = 1 if the
//         density at L is finite and positive, else 0 -- a kept sample is
//         exactly one `shade` would weight, and the rest are the ones it
//         discards; values[5i+4] = the weight f / pdf of a kept sample, the
//         mean of the response's three lanes, else 0.
//
// mode 1  quadrature. Stride 2, one entry per point inside one (theta, phi)
//         cell: values[2i] = pdf(L) sin(theta), values[2i+1] = f(L) sin(theta),
//         each multiplied by the host by the point's share of the cell's area.

layout(local_size_x = 64) in;

// Binding 0, because MaterialX's generated uniform block takes binding 1.
layout(set = 0, binding = 0, std430) buffer Values {
    float values[];
} results;

layout(push_constant) uniform Params {
    uint count;
    uint seed;
    float viewTheta;
    uint mode;
    uint thetaBins;
    uint phiBins;
    uint subdivisions;
    uint reserved;
} params;

uint pcg(inout uint state)
{
    state = state * 747796405u + 2891336453u;
    uint word = ((state >> ((state >> 28u) + 4u)) ^ state) * 277803737u;
    return (word >> 22u) ^ word;
}

float randomFloat(inout uint state)
{
    return float(pcg(state)) * (1.0 / 4294967296.0);
}

// Evaluate the closure at `L` the way `shade` asks -- a direction on the view's
// side of the surface is a reflection and anything else a transmission -- and
// return the density; the response is left in `hdclaude_bsdf`.
float evaluateAt(vec3 L, vec3 V, vec3 N, vec3 P)
{
    int closure = dot(L, N) * dot(V, N) > 0.0 ? CLOSURE_TYPE_REFLECTION
                                               : CLOSURE_TYPE_TRANSMISSION;
    ClosureData data = ClosureData(closure, L, V, N, P, 1.0);
    hdclaude_material_shade(data);
    return hdclaude_bsdf.pdf;
}

bool usable(float value)
{
    return !isnan(value) && !isinf(value);
}

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= params.count)
    {
        return;
    }

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 P = vec3(0.0);
    vec3 V = vec3(sin(params.viewTheta), 0.0, cos(params.viewTheta));

    hdclaude_set_surface_hit(P, N, vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), P,
                             N, vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                             vec2(0.5));
    hdclaude_wavelengths = vec4(450.0, 550.0, 600.0, 650.0);
    hdclaude_geometric_normal = N;
    hdclaude_inside_medium = params.viewTheta > 1.5707963 ? 1.0 : 0.0;

    if (params.mode == 0u)
    {
        uint rng = params.seed + index * 9781u;
        pcg(rng);
        pcg(rng);
        hdclaude_sample_u =
            vec3(randomFloat(rng), randomFloat(rng), randomFloat(rng));

        ClosureData sampleData =
            ClosureData(CLOSURE_TYPE_PT_SAMPLE, vec3(0.0), V, N, P, 1.0);
        hdclaude_material_shade(sampleData);
        vec3 L = hdclaude_bsdf.sampledL;

        uint base = index * 5u;
        results.values[base + 3u] = 0.0;
        results.values[base + 4u] = 0.0;
        if (dot(L, L) > 0.5)
        {
            L = normalize(L);
            float pdf = evaluateAt(L, V, N, P);
            vec3 f = hdclaude_bsdf.response;
            results.values[base + 0u] = L.x;
            results.values[base + 1u] = L.y;
            results.values[base + 2u] = L.z;
            if (usable(pdf) && pdf > 0.0)
            {
                results.values[base + 3u] = 1.0;
                float weight = (f.x + f.y + f.z) / (3.0 * pdf);
                results.values[base + 4u] = usable(weight) ? weight : 0.0;
            }
        }
        return;
    }

    // One quadrature point: a regular sub-grid inside each cell, at the centres
    // of its sub-cells.
    uint perCell = params.subdivisions * params.subdivisions;
    uint cell = index / perCell;
    uint point = index % perCell;
    uint thetaCell = cell / params.phiBins;
    uint phiCell = cell % params.phiBins;
    float thetaStep = 3.14159265358979 / float(params.thetaBins);
    float phiStep = 6.28318530717959 / float(params.phiBins);
    float theta = (float(thetaCell) +
                   (float(point / params.subdivisions) + 0.5) /
                       float(params.subdivisions)) * thetaStep;
    float phi = (float(phiCell) +
                 (float(point % params.subdivisions) + 0.5) /
                     float(params.subdivisions)) * phiStep;
    float s = sin(theta);
    vec3 L = vec3(s * cos(phi), s * sin(phi), cos(theta));

    float pdf = evaluateAt(L, V, N, P);
    vec3 f = hdclaude_bsdf.response;
    float response = (f.x + f.y + f.z) / 3.0;
    results.values[2u * index] = (usable(pdf) && pdf > 0.0) ? pdf * s : 0.0;
    results.values[2u * index + 1u] = usable(response) ? response * s : 0.0;
}
