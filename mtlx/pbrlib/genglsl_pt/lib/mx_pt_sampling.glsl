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
// Smith's masking G1 for the view, for the anisotropic distribution the
// visible-normal sampler actually draws from.
//
// The density of a VNDF sample is D_V(H) = G1(V) max(0, V.H) D(H) / N.V (Heitz
// 2018), and G1 there has to be the masking of *that* distribution. MaterialX's
// `mx_ggx_smith_G1(NdotV, alpha)` takes one roughness, and the closures passed
// it the geometric mean of the two; the sampler, meanwhile, stretches by each
// axis separately. For an isotropic lobe the two are the same formula. For an
// anisotropic one they are not: at 34 degrees along the smooth axis of a 0.2 by
// 0.6 lobe, 0.9953 against 0.9863, so every reported density was 0.9% short of
// the sampler's, every weight 0.9% high, and the furnace measured exactly that.
//
// `V` is in the tangent frame the sampler uses: x along the tangent, z along
// the normal. The response keeps MaterialX's own masking-shadowing, which is
// the model; only the density has to be the sampler's.
float mx_pt_ggx_smith_G1_anisotropic(vec3 V, vec2 alpha)
{
    float cos2 = V.z * V.z;
    if (cos2 <= 0.0)
    {
        return 0.0;
    }
    float a2tan2 = (alpha.x * alpha.x * V.x * V.x + alpha.y * alpha.y * V.y * V.y) / cos2;
    return 2.0 / (1.0 + sqrt(1.0 + a2tan2));
}

float mx_ggx_VNDF_reflection_PDF(vec3 H, vec2 alpha, float G1V, float NdotV)
{
    return mx_ggx_NDF(H, alpha) * G1V / (4.0 * NdotV);
}

// --- Refraction -------------------------------------------------------------

// Refract V about the microfacet normal H.
//
// `eta` is the relative index of refraction, incident over transmitted
// (eta_i / eta_t), matching GLSL's own `refract`. Returns false on total
// internal reflection, in which case `L` is not written -- the caller must
// reflect instead rather than continue with an undefined direction.
//
// Written out rather than calling `refract` so the TIR case is reported instead
// of silently returning a zero vector, which is indistinguishable from a valid
// direction downstream.
bool mx_pt_refract(vec3 V, vec3 H, float eta, out vec3 L)
{
    float cosThetaI = dot(H, V);
    float sin2ThetaI = max(0.0, 1.0 - cosThetaI * cosThetaI);
    float sin2ThetaT = eta * eta * sin2ThetaI;
    if (sin2ThetaT >= 1.0)
    {
        return false;
    }
    float cosThetaT = sqrt(1.0 - sin2ThetaT);
    L = eta * (-V) + (eta * cosThetaI - cosThetaT) * H;
    return true;
}

// Jacobian of the half-vector to incident-direction mapping, for transmission.
//
// This factor is what converts a density over microfacet normals into a density
// over directions, and it is the single easiest thing to get wrong in a rough
// dielectric: omitting it produces an image that looks plausible and is
// incorrectly weighted everywhere light refracts.
//
// `etaT` is transmitted over incident (eta_t / eta_i), the reciprocal of the
// value `mx_pt_refract` takes -- they are written with opposite conventions
// because each matches its own standard formulation, so the caller must pass
// the right one to each.
float mx_pt_refraction_jacobian(float VdotH, float LdotH, float etaT)
{
    float denom = VdotH + etaT * LdotH;
    denom = denom * denom;
    return denom > 0.0 ? (etaT * etaT * abs(LdotH)) / denom : 0.0;
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
