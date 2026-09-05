// Shared helpers for hdClaude's path-tracing closure queries.
//
// Deliberately thin: everything that MaterialX already provides is used from
// upstream rather than reimplemented, because a second implementation of GGX
// that disagrees with MaterialX's own is exactly the kind of divergence this
// project exists to avoid. `mx_ggx_importance_sample_VNDF`, `mx_ggx_NDF` and
// `mx_ggx_smith_G1` all come from
// pbrlib/genglsl/lib/mx_microfacet_specular.glsl unchanged.
//
// Version of record: MaterialX 1.39.3, the version inside OpenUSD 26.03.

#include "lib/mx_microfacet.glsl"
#include "lib/mx_microfacet_specular.glsl"

// --- Frames -----------------------------------------------------------------

// Build an orthonormal basis around N. Duff et al.'s branchless construction:
// no normalisation, no degenerate case at N.z = -1.
void mx_pt_basis(vec3 N, out vec3 X, out vec3 Y)
{
    float s = N.z >= 0.0 ? 1.0 : -1.0;
    float a = -1.0 / (s + N.z);
    float b = N.x * N.y * a;
    X = vec3(1.0 + s * N.x * N.x * a, s * b, -s * N.x);
    Y = vec3(b, s + N.y * N.y * a, -N.y);
}

vec3 mx_pt_to_world(vec3 local, vec3 X, vec3 Y, vec3 N)
{
    return local.x * X + local.y * Y + local.z * N;
}

vec3 mx_pt_to_local(vec3 world, vec3 X, vec3 Y, vec3 N)
{
    return vec3(dot(world, X), dot(world, Y), dot(world, N));
}

// --- Direction sampling -----------------------------------------------------

// Cosine-weighted hemisphere, via a concentric disc map. The concentric map is
// used rather than the polar one because it preserves sample stratification,
// which matters for the low sample counts an interactive frame runs at.
vec3 mx_pt_sample_cosine_hemisphere(vec2 Xi)
{
    vec2 offset = 2.0 * Xi - 1.0;
    if (offset.x == 0.0 && offset.y == 0.0)
    {
        return vec3(0.0, 0.0, 1.0);
    }

    float r;
    float theta;
    if (abs(offset.x) > abs(offset.y))
    {
        r = offset.x;
        theta = (M_PI / 4.0) * (offset.y / offset.x);
    }
    else
    {
        r = offset.y;
        theta = (M_PI / 2.0) - (M_PI / 4.0) * (offset.x / offset.y);
    }

    vec2 d = r * vec2(mx_cos(theta), mx_sin(theta));
    float z = sqrt(max(0.0, 1.0 - dot(d, d)));
    return vec3(d.x, d.y, z);
}

float mx_pt_cosine_hemisphere_pdf(float NdotL)
{
    return max(NdotL, 0.0) * M_PI_INV;
}

vec3 mx_pt_sample_uniform_sphere(vec2 Xi)
{
    float z = 1.0 - 2.0 * Xi.x;
    float r = sqrt(max(0.0, 1.0 - z * z));
    float phi = 2.0 * M_PI * Xi.y;
    return vec3(r * mx_cos(phi), r * mx_sin(phi), z);
}

float mx_pt_uniform_sphere_pdf()
{
    return 0.25 * M_PI_INV;
}

// --- GGX visible-normal density ---------------------------------------------

// MaterialX 1.39.3 provides mx_ggx_importance_sample_VNDF but not its density;
// 1.39.6 added mx_ggx_VNDF_reflection_PDF upstream. This is that function, with
// the same name so the definition can simply be deleted when the OpenUSD
// distribution moves to a MaterialX that supplies it.
//
// Expressed in terms of MaterialX's own mx_ggx_NDF and mx_ggx_smith_G1, so it
// cannot disagree with the distribution the sampler actually draws from.
float mx_ggx_VNDF_reflection_PDF(vec3 H, vec2 alpha, float G1V, float NdotV)
{
    return mx_ggx_NDF(H, alpha) * G1V / (4.0 * NdotV);
}

// --- Multiple importance sampling -------------------------------------------

// Power heuristic with beta = 2. `nf` and `ng` are the sample counts each
// strategy contributes, which are not always one once combinators split a
// closure into lobes.
float mx_pt_power_heuristic(float nf, float fPdf, float ng, float gPdf)
{
    float f = nf * fPdf;
    float g = ng * gPdf;
    float f2 = f * f;
    float denom = f2 + g * g;
    return denom > 0.0 ? f2 / denom : 0.0;
}

// Balance heuristic, used for wavelength MIS where the power heuristic's
// sharpening is not wanted: chromatic paths need the unbiased mixture, not a
// bias toward whichever lane happened to have the higher density.
float mx_pt_balance_heuristic(float fPdf, float gPdf)
{
    float denom = fPdf + gPdf;
    return denom > 0.0 ? fPdf / denom : 0.0;
}

// --- Combinator support -----------------------------------------------------

// One-sample stochastic selection between two lobes.
//
// Returns true to take the first lobe. `u` is consumed and *rescaled* back to
// [0, 1) so the caller can reuse it for the selected lobe's direction sample.
// Rescaling rather than drawing a fresh number keeps the sample count per
// bounce fixed, which is what allows a stratified sampler to stay stratified
// through an arbitrarily deep combinator tree.
bool mx_pt_select_lobe(inout float u, float probabilityFirst, out float selectionPdf)
{
    float p = clamp(probabilityFirst, 0.0, 1.0);
    if (u < p)
    {
        u = p > 0.0 ? u / p : 0.0;
        selectionPdf = p;
        return true;
    }
    float q = 1.0 - p;
    u = q > 0.0 ? (u - p) / q : 0.0;
    selectionPdf = q;
    return false;
}

// Average of a spectral or colour weight, used as a lobe-selection probability
// when the combinator has no authored weight to select with.
float mx_pt_luminance_weight(vec3 c)
{
    return max(dot(c, vec3(0.2126, 0.7152, 0.0722)), 0.0);
}
