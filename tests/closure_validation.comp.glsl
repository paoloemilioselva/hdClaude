// Numerical validation harness for a generated MaterialX material.
//
// This is appended to a generated material module, so it sees the ABI globals
// and `hdclaude_material_shade`. It answers the questions in
// docs/materialx-codegen.md 8 by brute force, on the GPU, in one dispatch per
// parameter point:
//
//   sum[0]  white furnace:  sum over sampled directions of f/pdf
//   sum[1]  sample count actually taken (some samples terminate below horizon)
//   sum[2]  mixture density integral: sum over uniform sphere of pdf * 4pi
//   sum[3]  uniform sample count
//   sum[4]  count of sampled directions whose reported pdf was non-finite
//   sum[5]  count of sampled directions whose reported pdf was zero
//
// The furnace sum divided by the sample count is the directional albedo, which
// must not exceed one: a closure that returns more energy than it receives is
// the failure this test exists to catch. The density integral must be one,
// which is what catches a combinator reporting a selected child's density
// instead of the mixture -- an error invisible to the furnace test, because the
// weight f/pdf is self-consistent in that case and only the *distribution* is
// wrong.
//
// Accumulated in fixed point through 32-bit atomicAdd. A float atomicAdd is an
// extension and its summation order is not reproducible; 64-bit atomics would
// need an #extension line, which cannot appear here because this text is
// appended *after* a generated material and GLSL requires extension directives
// before any code. The scale and the sample count are chosen together so the
// sums cannot overflow 32 bits.

layout(local_size_x = 64) in;

// Binding 0: MaterialX's generated uniform block takes binding 1, assigned by
// VkResourceBindingContext, so the harness takes 0 to stay out of its way.
layout(set = 0, binding = 0, std430) buffer Results {
    uint sums[8];
} results;

layout(push_constant) uniform Params {
    uint sampleCount;
    uint seed;
    float viewTheta;   // incidence angle, radians
    uint closureType;  // REFLECTION or TRANSMISSION for the furnace pass
} params;

// Fixed-point steps per unit.
//
// The bound is scale * sampleCount * meanValue < 2^32. At 2^20 samples and a
// mean near one, scale 512 accumulates to about 5.4e8 with room for means well
// above one; scale 4096 would reach 4.3e9 and wrap.
//
// That is not hypothetical: raising the sample count from 2^18 to 2^20 without
// lowering the scale wrapped this accumulator, and the wrapped result was a
// plausible-looking 0.0056 rather than an obvious garbage value. The host
// checks for saturation rather than trusting this arithmetic to stay right.
const float kFixedScale = 512.0;

// Values far above one indicate a closure that is already failing; they are
// clamped so a single pathological sample cannot wrap the accumulator and turn
// a loud failure into a plausible-looking number.
const float kMaxContribution = 64.0;

void accumulate(uint slot, float value)
{
    // A closure must not return negative energy. Counted rather than clamped
    // silently, so the test can report it.
    if (value < 0.0)
    {
        atomicAdd(results.sums[6], 1u);
        return;
    }
    atomicAdd(results.sums[slot], uint(min(value, kMaxContribution) * kFixedScale));
}

// PCG hash. Stateless, so a thread can produce any dimension without carrying
// sampler state -- the property docs/wavefront-integrator.md 5 relies on.
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

void main()
{
    uint index = gl_GlobalInvocationID.x;
    if (index >= params.sampleCount)
    {
        return;
    }

    uint rng = params.seed + index * 9781u;
    // Warm up, so neighbouring indices do not start correlated.
    pcg(rng);
    pcg(rng);

    vec3 N = vec3(0.0, 0.0, 1.0);
    vec3 P = vec3(0.0);
    vec3 V = vec3(sin(params.viewTheta), 0.0, cos(params.viewTheta));

    // The generator emits this with exactly the assignments this material's
    // geometry needs; a diffuse-only material has no tangent to write.
    hdclaude_set_surface_hit(P, N, vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0), P,
                             N, vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0),
                             vec2(0.5));
    hdclaude_wavelengths = vec4(450.0, 550.0, 600.0, 650.0);
    // A flat surface, so the geometric and shading normals are the same one.
    // Supplying it is what makes `viewTheta` past 90 degrees mean "inside" to a
    // closure rather than "a silhouette".
    hdclaude_geometric_normal = N;
    // A view direction behind the surface is declared to be inside the medium,
    // which is what the integrator would have recorded for a path that got
    // there by refracting in. Without it the inside probes would measure the
    // outside curve and pass for the wrong reason.
    hdclaude_inside_medium = params.viewTheta > 1.5707963 ? 1.0 : 0.0;

    // --- Pass A: sample a direction, then evaluate f and pdf at it -----------
    hdclaude_sample_u = vec3(randomFloat(rng), randomFloat(rng), randomFloat(rng));

    ClosureData sampleData =
        ClosureData(CLOSURE_TYPE_PT_SAMPLE, vec3(0.0), V, N, P, 1.0);
    hdclaude_material_shade(sampleData);
    vec3 L = hdclaude_bsdf.sampledL;

    if (dot(L, L) > 0.5)
    {
        L = normalize(L);
        ClosureData evalData = ClosureData(int(params.closureType), L, V, N, P, 1.0);
        hdclaude_material_shade(evalData);

        float pdf = hdclaude_bsdf.pdf;
        vec3 f = hdclaude_bsdf.response;

        if (isnan(pdf) || isinf(pdf) || isnan(f.x) || isinf(f.x))
        {
            atomicAdd(results.sums[4], 1u);
        }
        else if (pdf <= 0.0)
        {
            // A direction the sampler produced but the density says is
            // impossible. Every such sample is a path the estimator must
            // discard, and a large count means the sampler and the density
            // disagree about the lobe's support.
            atomicAdd(results.sums[5], 1u);
        }
        else
        {
            // f already contains the cosine factor, matching MaterialX's
            // convention that `response` is f * cos.
            float weight = (f.x + f.y + f.z) / 3.0 / pdf;
            accumulate(0, weight);
        }
        atomicAdd(results.sums[1], 1u);
    }

    // --- Pass B: integrate the reported density over the sphere -------------
    // Uniform directions, so the Monte Carlo estimate of the integral is
    // mean(pdf) * 4pi and must come to one for a normalised density.
    float z = 1.0 - 2.0 * randomFloat(rng);
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 6.283185307 * randomFloat(rng);
    vec3 uniformL = vec3(r * cos(phi), r * sin(phi), z);

    ClosureData densityData =
        ClosureData(int(params.closureType), uniformL, V, N, P, 1.0);
    hdclaude_material_shade(densityData);

    float updf = hdclaude_bsdf.pdf;
    if (!isnan(updf) && !isinf(updf) && updf > 0.0)
    {
        accumulate(2, updf * 12.56637061);
    }
    atomicAdd(results.sums[3], 1u);
}
