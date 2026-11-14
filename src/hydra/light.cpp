#include "light.h"

#include "render_param.h"
#include "scene_store.h"
#include "texture_loader.h"
#include "trace.h"

#include "hdclaude/core/spectrum.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec2d.h"
#include "pxr/base/gf/vec2f.h"
#include "pxr/base/tf/stringUtils.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/usdLux/tokens.h"

#include <algorithm>
#include <cmath>
#include <type_traits>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

/// Read a light parameter, falling back when the scene does not author it.
template <typename T>
T Param(HdSceneDelegate* delegate, const SdfPath& id, const TfToken& name,
        const T& fallback)
{
    const VtValue value = delegate->GetLightParamValue(id, name);
    if (value.IsHolding<T>()) {
        return value.UncheckedGet<T>();
    }
    // Authored as a double where the schema says float, or the reverse, is
    // common enough in real assets to be worth casting rather than dropping.
    // `if constexpr` because the conversion is only well formed for the
    // arithmetic instantiations; SdfAssetPath and bool must not see it.
    if constexpr (std::is_same_v<T, float>) {
        if (value.IsHolding<double>()) {
            return static_cast<T>(value.UncheckedGet<double>());
        }
    } else if constexpr (std::is_same_v<T, double>) {
        if (value.IsHolding<float>()) {
            return static_cast<T>(value.UncheckedGet<float>());
        }
    }
    return fallback;
}

GfVec3f ParamColor(HdSceneDelegate* delegate, const SdfPath& id,
                   const TfToken& name, const GfVec3f& fallback)
{
    const VtValue value = delegate->GetLightParamValue(id, name);
    if (value.IsHolding<GfVec3f>()) {
        return value.UncheckedGet<GfVec3f>();
    }
    if (value.IsHolding<GfVec3d>()) {
        const GfVec3d asDouble = value.UncheckedGet<GfVec3d>();
        return GfVec3f(asDouble[0], asDouble[1], asDouble[2]);
    }
    return fallback;
}

void StoreVector(float (&out)[3], const GfVec3f& value)
{
    out[0] = value[0];
    out[1] = value[1];
    out[2] = value[2];
}

/// The category a light-linking parameter names, or the empty token Hydra uses
/// for a collection that includes everything.
TfToken LinkParam(HdSceneDelegate* delegate, const SdfPath& id,
                  const TfToken& name)
{
    const VtValue value = delegate->GetLightParamValue(id, name);
    return value.IsHolding<TfToken>() ? value.UncheckedGet<TfToken>() : TfToken();
}

/// Appends "`name` = value is not honoured" to a light's report.
void Unhonoured(std::string* report, const std::string& what)
{
    if (!report->empty()) {
        *report += "; ";
    }
    *report += what;
}

/// The UsdLux inputs hdClaude reads nothing of, reported by name when they are
/// authored away from the value that would make ignoring them exact.
///
/// None of these is substituted for. UsdLux calls `diffuse` and `specular`
/// "non-physical" controls, `shadow:color`, `distance` and `falloff` are
/// artistic ones, and a physically based transport has no place to put any of
/// them that would still be the same light; a light filter is a shader
/// hdClaude has no implementation of. Saying so is the whole of the support.
std::string UnhonouredInputs(HdSceneDelegate* delegate, const SdfPath& id)
{
    std::string report;
    const auto scalar = [&](const TfToken& name, float exact) {
        const float value = Param<float>(delegate, id, name, exact);
        if (value != exact) {
            Unhonoured(&report, TfStringPrintf("inputs:%s = %g is not honoured "
                                               "(treated as %g)",
                                               name.GetText(), value, exact));
        }
    };
    scalar(HdLightTokens->diffuse, 1.0f);
    scalar(HdLightTokens->specular, 1.0f);
    scalar(HdLightTokens->shadowDistance, -1.0f);
    scalar(HdLightTokens->shadowFalloff, -1.0f);
    scalar(HdLightTokens->shadowFalloffGamma, 1.0f);

    const auto colour = [&](const TfToken& name) {
        const GfVec3f value = ParamColor(delegate, id, name, GfVec3f(0.0f));
        if (value != GfVec3f(0.0f)) {
            Unhonoured(&report,
                       TfStringPrintf("inputs:%s = (%g, %g, %g) is not honoured "
                                      "(treated as black)",
                                      name.GetText(), value[0], value[1], value[2]));
        }
    };
    colour(HdLightTokens->shadowColor);
    colour(HdLightTokens->shapingFocusTint);

    // `treatAsPoint` is not an `inputs:` property: UsdLux declares it as a plain
    // uniform bool on the sphere light, so it is read by its own name.
    //
    // Authored true, it says to shade the light as a point emitter rather than
    // as a sphere of the authored radius -- which is a different light, with
    // hard shadow edges instead of a penumbra the radius sets. hdClaude samples
    // the sphere either way, so this says so rather than quietly rendering the
    // softer light. The same goes for `treatAsLine` on a cylinder light, which
    // is reported for the same reason even though hdClaude has no cylinder
    // light at all yet and says so separately.
    const auto asPoint = [&](const TfToken& name, const char* shape) {
        if (Param<bool>(delegate, id, name, false)) {
            Unhonoured(&report,
                       TfStringPrintf("%s = 1 is not honoured; the light is "
                                      "sampled as %s of its authored size",
                                      name.GetText(), shape));
        }
    };
    asPoint(UsdLuxTokens->treatAsPoint, "a sphere");
    asPoint(UsdLuxTokens->treatAsLine, "a cylinder");

    const VtValue filters = delegate->GetLightParamValue(id, HdTokens->filters);
    if (filters.IsHolding<SdfPathVector>() &&
        !filters.UncheckedGet<SdfPathVector>().empty()) {
        Unhonoured(&report,
                   TfStringPrintf("light:filters names %zu light filter(s), and "
                                  "hdClaude implements no light filter",
                                  filters.UncheckedGet<SdfPathVector>().size()));
    }
    return report;
}

/// A light parameter by any of the names it can arrive under.
///
/// UsdImaging hands a light's `inputs:` properties to a render delegate by their
/// base name, and its other properties by their full name; the draft
/// physical-lighting schema's attributes are not `inputs:`, but an author may
/// write them as inputs, and an unregistered schema gives no declaration to
/// settle it. So each name is asked for as written and with `inputs:` added.
VtValue ParamAnyName(HdSceneDelegate* delegate, const SdfPath& id,
                     const std::string& name)
{
    for (const std::string& candidate : {name, "inputs:" + name}) {
        VtValue value = delegate->GetLightParamValue(id, TfToken(candidate));
        if (!value.IsEmpty()) {
            return value;
        }
    }
    return {};
}

/// The illuminant a light authors through the draft UsdLux
/// `PhysicalLightIlluminantAPI` (docs/usd-proposals.md), if it authors one.
///
/// `physical:illuminant` picks the spectrum the light's `color` is referred to:
///
///   white         the rendering colour space's white point, D65 here
///   blackbody     Planck's law at `inputs:colorTemperature`
///   illuminant D  CIE daylight at `inputs:colorTemperature` (D65 is 6504 K)
///   custom        `physical:customIlluminant`, (nm, radiance) pairs, zero
///                 outside their range, resampled to 5 nm per ASTM E308
///
/// "white", "blackbody" and "illuminant D" are tabulated at 5 nm over 380-780,
/// where the proposal defines the first two. Each becomes a tabulated spectrum,
/// so one path renders all four. `color` still tints it and `intensity`,
/// `exposure` and `normalize` still scale it; the spectrum is scaled to D65's
/// luminous power, so a light's luminance is its colour's times its intensity
/// whichever illuminant it has -- the proposal's definition.
///
/// The schema is a proposal and not registered, so whether it is applied is
/// read from whether `physical:illuminant` has a value -- or from `implied`,
/// when the light authors one of the photometric or radiometric APIs, which
/// inherit this one, and then the illuminant is the schema's fallback,
/// "white". Returns false when it is not applied, and the light keeps UsdLux's
/// own colour temperature. An
/// illuminant that is authored and unusable is reported and not used, and the
/// light falls back to its colour against D65: the colour is the portable
/// statement of the light, the one every other renderer reads.
bool ReadPhysicalIlluminant(HdSceneDelegate* delegate, const SdfPath& id,
                            bool implied, hdclaude::TabulatedSpectrum* spectrum,
                            std::string* report)
{
    const VtValue illuminantValue = ParamAnyName(delegate, id, "physical:illuminant");
    if (illuminantValue.IsEmpty() && !implied) {
        return false;
    }
    std::string illuminant;
    if (illuminantValue.IsEmpty()) {
        illuminant = "white";
    } else if (illuminantValue.IsHolding<TfToken>()) {
        illuminant = illuminantValue.UncheckedGet<TfToken>().GetString();
    } else if (illuminantValue.IsHolding<std::string>()) {
        illuminant = illuminantValue.UncheckedGet<std::string>();
    } else {
        Unhonoured(report, "physical:illuminant is a " +
                               illuminantValue.GetTypeName() +
                               " where the proposal declares a token; the light "
                               "emits its colour");
        return false;
    }

    const float kelvin =
        Param<float>(delegate, id, HdLightTokens->colorTemperature, 6500.0f);
    hdclaude::TabulatedSpectrum candidate;
    if (illuminant == "white") {
        candidate = hdclaude::WhiteIlluminant();
    } else if (illuminant == "blackbody") {
        candidate = hdclaude::BlackbodyIlluminant(kelvin);
        if (candidate.wavelengths.empty()) {
            Unhonoured(report, TfStringPrintf(
                                   "physical:illuminant is blackbody at %g K, which "
                                   "has no spectrum; the light emits its colour",
                                   kelvin));
            return false;
        }
    } else if (illuminant == "illuminant D") {
        if (!hdclaude::IlluminantDDefined(kelvin)) {
            Unhonoured(report,
                       TfStringPrintf("physical:illuminant is illuminant D at %g K, "
                                      "outside the 4000-25000 K over which CIE "
                                      "defines daylight; the light emits its colour",
                                      kelvin));
            return false;
        }
        candidate = hdclaude::DaylightIlluminant(kelvin);
    } else if (illuminant == "custom") {
        const VtValue pairs = ParamAnyName(delegate, id, "physical:customIlluminant");
        hdclaude::TabulatedSpectrum authored;
        if (pairs.IsHolding<VtVec2fArray>()) {
            for (const GfVec2f& pair : pairs.UncheckedGet<VtVec2fArray>()) {
                authored.wavelengths.push_back(pair[0]);
                authored.values.push_back(pair[1]);
            }
        } else if (pairs.IsHolding<VtVec2dArray>()) {
            for (const GfVec2d& pair : pairs.UncheckedGet<VtVec2dArray>()) {
                authored.wavelengths.push_back(static_cast<float>(pair[0]));
                authored.values.push_back(static_cast<float>(pair[1]));
            }
        } else if (!pairs.IsEmpty()) {
            Unhonoured(report, "physical:customIlluminant is a " + pairs.GetTypeName() +
                                   " where the proposal declares float2[]; the "
                                   "light emits its colour");
            return false;
        }
        const std::string invalid = hdclaude::ValidateTabulatedSpectrum(authored, false);
        if (!invalid.empty()) {
            Unhonoured(report, "physical:illuminant is custom, and "
                               "physical:customIlluminant " + invalid +
                               "; the light emits its colour");
            return false;
        }
        int negatives = 0;
        candidate = hdclaude::ResampleTo5nm(authored, &negatives);
        if (negatives > 0) {
            // A property of the resampling the proposal prescribes, not of the
            // data; said, because the rendered spectrum is then not exactly
            // the interpolant.
            Unhonoured(report,
                       TfStringPrintf("resampling physical:customIlluminant to 5 nm "
                                      "dips below zero at %d wavelength(s), where "
                                      "the emission is taken as zero",
                                      negatives));
        }
    } else {
        Unhonoured(report, "physical:illuminant = \"" + illuminant +
                               "\" is not one of white, blackbody, illuminant D "
                               "or custom; the light emits its colour");
        return false;
    }

    if (!(hdclaude::TabulatedLuminousScale(candidate) > 0.0f)) {
        Unhonoured(report, "physical:illuminant emits nothing the eye can see, so "
                           "it has no luminous power to scale to and is not used");
        return false;
    }
    if (Param<bool>(delegate, id, HdLightTokens->enableColorTemperature, false) &&
        illuminant != "blackbody" && illuminant != "illuminant D") {
        Unhonoured(report, "inputs:enableColorTemperature is not honoured, because "
                           "physical:illuminant = " + illuminant +
                           " says what the light emits");
    }
    *spectrum = std::move(candidate);
    return true;
}

/// UsdLux's `sizeFactor` for a distant light with `normalize` on.
///
/// Quoted from `UsdLuxLightAPI::GetNormalizeAttr`: with the half-angle
/// `theta = clamp(radians(angle) / 2, 0, pi)`, the factor is 1 at zero,
/// `pi sin^2 theta` up to a hemisphere and `(2 - sin^2 theta) pi` beyond it. It
/// is the solid angle's projection onto a surface facing the light, so a
/// normalised distant light delivers `intensity` lux there whatever its size.
float DistantSizeFactor(float angleDegrees)
{
    const float theta = std::clamp(
        angleDegrees * static_cast<float>(M_PI) / 180.0f * 0.5f, 0.0f,
        static_cast<float>(M_PI));
    if (theta <= 0.0f) {
        return 1.0f;
    }
    const float sin2 = std::sin(theta) * std::sin(theta);
    return theta <= 0.5f * static_cast<float>(M_PI)
               ? static_cast<float>(M_PI) * sin2
               : (2.0f - sin2) * static_cast<float>(M_PI);
}

/// The draft photometric and radiometric APIs' attributes, by light kind.
///
/// An area light's API has a power and an illuminance at a distance; a dome's
/// and a distant light's have only an illuminance. The fallbacks are the
/// draft's.
struct BrightnessAttribute {
    const char* name;
    float fallback;
};
constexpr BrightnessAttribute kAreaPhotometric[] = {
    {"photometric:power", 1600.0f},
    {"photometric:illuminance", 0.0f},
    {"photometric:illuminance:distance", 0.0f}};
constexpr BrightnessAttribute kAreaRadiometric[] = {
    {"radiometric:power", 2.34f},
    {"radiometric:irradiance", 0.0f},
    {"radiometric:irradiance:distance", 0.0f}};
constexpr BrightnessAttribute kDomePhotometric[] = {{"photometric:illuminance", 10000.0f}};
constexpr BrightnessAttribute kDomeRadiometric[] = {{"radiometric:irradiance", 14.64f}};

/// Every attribute either kind of API could author, on any light.
constexpr const char* kBrightnessNames[] = {
    "photometric:power", "photometric:illuminance", "photometric:illuminance:distance",
    "radiometric:power", "radiometric:irradiance", "radiometric:irradiance:distance"};

/// Whether the light authors any of the photometric or radiometric APIs'
/// attributes, which is how an unregistered applied schema is recognised.
bool AuthorsBrightness(HdSceneDelegate* delegate, const SdfPath& id)
{
    for (const char* name : kBrightnessNames) {
        if (!ParamAnyName(delegate, id, name).IsEmpty()) {
            return true;
        }
    }
    return false;
}

/// One of those attributes: its value if authored as a number, its fallback
/// if not authored. False, with a report, if it is authored as something else.
bool BrightnessValue(HdSceneDelegate* delegate, const SdfPath& id,
                     const BrightnessAttribute& attribute, float* value,
                     bool* authored, std::string* report)
{
    const VtValue v = ParamAnyName(delegate, id, attribute.name);
    *authored = !v.IsEmpty();
    if (v.IsEmpty()) {
        *value = attribute.fallback;
        return true;
    }
    if (v.IsHolding<float>()) {
        *value = v.UncheckedGet<float>();
        return true;
    }
    if (v.IsHolding<double>()) {
        *value = static_cast<float>(v.UncheckedGet<double>());
        return true;
    }
    Unhonoured(report, std::string(attribute.name) + " is a " + v.GetTypeName() +
                           " where the proposal declares a float; the light emits "
                           "as authored");
    return false;
}

/// What a light is authored to deliver through the draft UsdLux
/// `PhotometricAreaLightAPI`, `RadiometricAreaLightAPI`,
/// `Photometric/RadiometricDomeLightAPI` or
/// `Photometric/RadiometricDistantLightAPI` (docs/usd-proposals.md).
///
/// None is registered, so an API counts as applied when any of its
/// attributes has a value, and its unauthored attributes take the draft's
/// fallbacks. Where the draft decides, this follows it: the photometric API
/// wins over the radiometric one, and an area light's illuminance counts only
/// when its distance is not zero. Where it is silent, this decides, and
/// usd-proposals.md says so: at a non-zero distance the illuminance wins over
/// the power, as the distance's own documentation reads ("When non-zero, the
/// light's brightness will be such that ...").
///
/// A radiometric target is converted to the photometric one through the
/// light's illuminant -- the spectrum it is "integrated over the domain of" --
/// so the luminance is scaled the same way for both.
HdClaudeLightBrightness ReadBrightness(HdSceneDelegate* delegate, const SdfPath& id,
                                       bool area,
                                       const hdclaude::TabulatedSpectrum& illuminant,
                                       std::string* report)
{
    HdClaudeLightBrightness brightness;
    const auto applied = [&](const BrightnessAttribute* attributes, std::size_t count) {
        for (std::size_t i = 0; i < count; ++i) {
            if (!ParamAnyName(delegate, id, attributes[i].name).IsEmpty()) {
                return true;
            }
        }
        return false;
    };
    // A dome or distant light has no power, and no distance to measure at.
    if (!area) {
        for (const char* name : kBrightnessNames) {
            const std::string text(name);
            const bool areaOnly = text.find("power") != std::string::npos ||
                                  text.find(":distance") != std::string::npos;
            if (areaOnly && !ParamAnyName(delegate, id, name).IsEmpty()) {
                Unhonoured(report, text + " applies to area lights only, and is "
                                          "not used on a dome or distant light");
            }
        }
    }

    const bool photometric = area ? applied(kAreaPhotometric, 3)
                                  : applied(kDomePhotometric, 1);
    const bool radiometric = area ? applied(kAreaRadiometric, 3)
                                  : applied(kDomeRadiometric, 1);
    if (!photometric && !radiometric) {
        return brightness;
    }
    if (photometric && radiometric) {
        Unhonoured(report, "the radiometric API is not used, because the "
                           "photometric one is authored too and takes precedence");
    }
    const char* powerUnits = photometric ? "lm" : "W";
    const char* fluxUnits = photometric ? "lux" : "W/m^2";

    float value = 0.0f;
    if (area) {
        const BrightnessAttribute* attributes =
            photometric ? kAreaPhotometric : kAreaRadiometric;
        float power = 0.0f, illuminance = 0.0f, distance = 0.0f;
        bool powerAuthored = false, illuminanceAuthored = false, distanceAuthored = false;
        if (!BrightnessValue(delegate, id, attributes[0], &power, &powerAuthored, report) ||
            !BrightnessValue(delegate, id, attributes[1], &illuminance,
                             &illuminanceAuthored, report) ||
            !BrightnessValue(delegate, id, attributes[2], &distance, &distanceAuthored,
                             report)) {
            return brightness;
        }
        if (distance < 0.0f || !std::isfinite(distance)) {
            Unhonoured(report, TfStringPrintf("%s = %g is not a distance; the light "
                                              "emits as authored",
                                              attributes[2].name, distance));
            return brightness;
        }
        if (distance > 0.0f) {
            if (powerAuthored) {
                Unhonoured(report, TfStringPrintf("%s is not used, because %s is "
                                                  "not zero",
                                                  attributes[0].name, attributes[2].name));
            }
            brightness.target = HdClaudeLightBrightness::Target::Illuminance;
            brightness.distanceMeters = distance;
            value = illuminance;
            brightness.statement =
                TfStringPrintf("%s = %g %s at %s = %g m", attributes[1].name, illuminance,
                               fluxUnits, attributes[2].name, distance);
        } else {
            if (illuminanceAuthored) {
                Unhonoured(report, TfStringPrintf("%s has no effect while %s is "
                                                  "zero, so %s is used",
                                                  attributes[1].name, attributes[2].name,
                                                  attributes[0].name));
            }
            brightness.target = HdClaudeLightBrightness::Target::Power;
            value = power;
            brightness.statement = TfStringPrintf(
                "%s = %g %s%s", attributes[0].name, power, powerUnits,
                powerAuthored ? "" : " (the draft's fallback)");
        }
    } else {
        const BrightnessAttribute& attribute =
            photometric ? kDomePhotometric[0] : kDomeRadiometric[0];
        bool authored = false;
        if (!BrightnessValue(delegate, id, attribute, &value, &authored, report)) {
            return brightness;
        }
        brightness.target = HdClaudeLightBrightness::Target::Illuminance;
        brightness.statement =
            TfStringPrintf("%s = %g %s%s", attribute.name, value, fluxUnits,
                           authored ? "" : " (the draft's fallback)");
    }

    if (value < 0.0f || !std::isfinite(value)) {
        Unhonoured(report, brightness.statement +
                               " is not a quantity of light; the light emits as authored");
        return HdClaudeLightBrightness();
    }
    brightness.value = value;
    if (!photometric) {
        // Watts to lumens: the light's luminance is what gets scaled, and its
        // radiance per nit is fixed by its spectrum.
        const double radiantPerLuminous = hdclaude::TabulatedRadiantPerLuminous(illuminant);
        if (!(radiantPerLuminous > 0.0)) {
            Unhonoured(report, brightness.statement +
                                   " cannot be met, because the illuminant has no "
                                   "luminous power; the light emits as authored");
            return HdClaudeLightBrightness();
        }
        brightness.value = value / radiantPerLuminous;
        brightness.statement +=
            TfStringPrintf(" (%g %s through the illuminant)", brightness.value,
                           brightness.target == HdClaudeLightBrightness::Target::Power
                               ? "lm"
                               : "lux");
    }
    return brightness;
}

/// D65 over the whole range a light without an illuminant emits it on, at the
/// illuminant grid: what a radiometric target is integrated over when the
/// light's illuminant was unusable and it fell back to its colour.
hdclaude::TabulatedSpectrum FullRangeD65()
{
    hdclaude::TabulatedSpectrum d65;
    for (float lambda = hdclaude::kLambdaMin; lambda <= hdclaude::kLambdaMax;
         lambda += 5.0f) {
        d65.wavelengths.push_back(lambda);
        d65.values.push_back(hdclaude::IlluminantD65(lambda));
    }
    return d65;
}

}  // namespace

HdClaudeLight::HdClaudeLight(const TfToken& lightType, const SdfPath& id)
    : HdLight(id), _lightType(lightType)
{
}

HdClaudeLight::~HdClaudeLight() = default;

HdDirtyBits HdClaudeLight::GetInitialDirtyBitsMask() const
{
    return HdLight::AllDirty;
}

void HdClaudeLight::Finalize(HdRenderParam* renderParam)
{
    if (auto* param = static_cast<HdClaudeRenderParam*>(renderParam)) {
        param->SceneStore()->RemoveLight(GetId());
    }
}

void HdClaudeLight::Sync(HdSceneDelegate* sceneDelegate,
                         HdRenderParam* renderParam, HdDirtyBits* dirtyBits)
{
    auto* param = static_cast<HdClaudeRenderParam*>(renderParam);
    if (param == nullptr || sceneDelegate == nullptr) {
        *dirtyBits = HdLight::Clean;
        return;
    }
    const SdfPath& id = GetId();

    if (!sceneDelegate->GetVisible(id)) {
        param->SceneStore()->RemoveLight(id);
        *dirtyBits = HdLight::Clean;
        return;
    }

    // --- Emitted radiance -----------------------------------------------------
    // colour * intensity * 2^exposure, the UsdLux definition.
    const float intensity =
        Param<float>(sceneDelegate, id, HdLightTokens->intensity, 1.0f);
    const float exposure =
        Param<float>(sceneDelegate, id, HdLightTokens->exposure, 0.0f);
    const GfVec3f color =
        ParamColor(sceneDelegate, id, HdLightTokens->color, GfVec3f(1.0f));
    const bool normalize =
        Param<bool>(sceneDelegate, id, HdLightTokens->normalize, false);

    GfVec3f radiance = color * intensity * std::pow(2.0f, exposure);

    // A colour temperature is carried to the GPU as a temperature, not
    // resolved to a tint here.
    //
    // The two are not the same thing. An RGB tint says "multiply the light's
    // colour by the blackbody's colour", which is a metamer of the real
    // spectrum and behaves like one: it lights a surface whose reflectance
    // varies across the spectrum -- which is every real surface -- differently
    // from the blackbody it stands for. Transporting the spectrum is the whole
    // reason for the four lanes, and this is the input that most obviously
    // needs it.
    //
    // The scale that comes with it equates the blackbody's luminous integral
    // with the default illuminant's, so enabling the control changes hue and
    // not brightness. That is the same promise the RGB tint made by
    // normalising to unit luminance; it is now kept spectrally.
    float colorTemperature = 0.0f;
    float temperatureScale = 1.0f;
    if (Param<bool>(sceneDelegate, id, HdLightTokens->enableColorTemperature,
                    false)) {
        colorTemperature = Param<float>(
            sceneDelegate, id, HdLightTokens->colorTemperature, 6500.0f);
        temperatureScale = hdclaude::BlackbodyLuminousScale(colorTemperature);
    }

    // --- Placement -------------------------------------------------------------
    GfMatrix4d transform = sceneDelegate->GetTransform(id);
    const GfVec3f position(transform.ExtractTranslation());
    // USD lights emit along their local -Z.
    const GfVec3f emitDirection =
        GfVec3f(transform.TransformDir(GfVec3d(0.0, 0.0, -1.0))).GetNormalized();

    std::string entryReport = UnhonouredInputs(sceneDelegate, id);
    const TfToken lightLink = LinkParam(sceneDelegate, id, HdTokens->lightLink);
    const TfToken shadowLink = LinkParam(sceneDelegate, id, HdTokens->shadowLink);

    // The draft physical-lighting illuminant replaces whichever illuminant the
    // colour would have been referred to, on every kind of light. Its place in
    // the scene's spectrum table is assigned at snapshot; only the luminous
    // scale is known here.
    hdclaude::TabulatedSpectrum emissionSpectrum;
    const bool authorsBrightness = AuthorsBrightness(sceneDelegate, id);
    if (ReadPhysicalIlluminant(sceneDelegate, id, authorsBrightness, &emissionSpectrum,
                               &entryReport)) {
        colorTemperature = 0.0f;
        temperatureScale = hdclaude::TabulatedLuminousScale(emissionSpectrum);
    }

    // What the light is authored to deliver, if anything, through the draft
    // photometric and radiometric APIs. Resolved into a scale at snapshot,
    // where the stage's units are known (HdClaudeLightBrightness).
    const bool areaLight = _lightType == HdPrimTypeTokens->rectLight ||
                           _lightType == HdPrimTypeTokens->diskLight ||
                           _lightType == HdPrimTypeTokens->sphereLight ||
                           _lightType == HdPrimTypeTokens->cylinderLight;
    HdClaudeLightBrightness brightness;
    if (authorsBrightness) {
        brightness = ReadBrightness(
            sceneDelegate, id, areaLight,
            emissionSpectrum.wavelengths.empty() ? FullRangeD65() : emissionSpectrum,
            &entryReport);
    }

    hdclaude::Light light;
    light.colorTemperature = colorTemperature;
    light.temperatureScale = temperatureScale;
    StoreVector(light.position, position);
    StoreVector(light.direction, emitDirection);
    light.castsShadows =
        Param<bool>(sceneDelegate, id, HdLightTokens->shadowEnable, true) ? 1u : 0u;
    if (light.castsShadows == 0u) {
        // Honoured exactly as UsdLux defines it -- nothing occludes such a
        // light -- and reported, because it is the one light input that can
        // change a whole image without looking like it did: an unshadowed
        // light passes through every wall and does it again at every bounce.
        //
        // It is reported for a second reason. A valueless `inputs:shadow:enable`
        // is "no opinion", which UsdLux says falls back to true, and
        // UsdImaging's attribute data source zero-initialises instead, so such
        // a light arrives here indistinguishable from one authored `false`
        // (docs/implementation-notes.md, 2026-09-16). hdClaude cannot tell
        // them apart and must not guess; saying which lights are unshadowed is
        // what lets the scene be checked.
        Unhonoured(&entryReport,
                   "inputs:shadow:enable is false, so no geometry occludes this "
                   "light -- if the scene did not mean that, check whether the "
                   "attribute is authored with a value at all");
    }

    if (_lightType == HdPrimTypeTokens->domeLight) {
        // Not an emitter in the light table: the environment a ray sees when it
        // leaves the scene, sampled by the environment strategy instead.
        HdClaudeLightEntry entry;
        entry.isDome = true;
        entry.lightLink = lightLink;
        entry.shadowLink = shadowLink;
        entry.domeColorTemperature = colorTemperature;
        entry.domeTemperatureScale = temperatureScale;
        entry.emissionSpectrum = emissionSpectrum;
        entry.brightness = brightness;
        StoreVector(entry.environmentColor, radiance);

        // The map is loaded through the same pool as a material's textures, so
        // a dome sharing an image with a material costs one upload.
        const SdfAssetPath textureFile = Param<SdfAssetPath>(
            sceneDelegate, id, HdLightTokens->textureFile, SdfAssetPath());
        std::string texturePath = textureFile.GetResolvedPath();
        if (texturePath.empty()) {
            texturePath = textureFile.GetAssetPath();
        }
        if (!texturePath.empty() && param->TexturePool() != nullptr) {
            entry.domeTexture =
                static_cast<int>(param->TexturePool()->Acquire(texturePath));
        }

        // Only a latitude-longitude map is read. `automatic` is taken to be
        // one, since that is the only layout hdClaude can decode it as; a map
        // authored as any other layout would be sampled in the wrong
        // parameterisation, and says so.
        const TfToken format = Param<TfToken>(
            sceneDelegate, id, HdLightTokens->textureFormat, TfToken("automatic"));
        if (!texturePath.empty() && format != TfToken("automatic") &&
            format != TfToken("latlong")) {
            Unhonoured(&entryReport,
                       TfStringPrintf("inputs:texture:format = %s is read as "
                                      "latlong, the only layout hdClaude samples",
                                      format.GetText()));
        }
        const VtValue portals =
            sceneDelegate->GetLightParamValue(id, HdTokens->portals);
        if (portals.IsHolding<SdfPathVector>() &&
            !portals.UncheckedGet<SdfPathVector>().empty()) {
            // Portals guide sampling and change no expected value, so ignoring
            // them costs noise, not correctness -- but they were asked for.
            Unhonoured(&entryReport,
                       "portals are not used to guide the dome's sampling");
        }
        entry.report = entryReport;

        // `DomeLight_1`'s pole axis. UsdImaging hands it over as `domeOffset`,
        // a rotation that aligns the map's top pole with the stage's up axis
        // (or the axis the light names) and that applies to the dome alone,
        // before the prim's own transform -- hdPrman composes it in the same
        // order. `DomeLight` has none, and the parameter is then absent.
        const VtValue domeOffset =
            sceneDelegate->GetLightParamValue(id, HdLightTokens->domeOffset);
        if (domeOffset.IsHolding<GfMatrix4d>()) {
            transform = domeOffset.UncheckedGet<GfMatrix4d>() * transform;
        }

        // The dome's own rotation, inverted: the environment kernel takes a
        // world direction into the map's frame, not the other way round.
        const GfMatrix4d worldToLight = transform.GetInverse();
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                entry.domeWorldToLight[column * 4 + row] =
                    static_cast<float>(worldToLight[column][row]);
                entry.domeLightToWorld[column * 4 + row] =
                    static_cast<float>(transform[column][row]);
            }
        }
        param->SceneStore()->PublishLight(id, std::move(entry));
        HdClaudeTrace("dome light <%s>: environment %.3f %.3f %.3f, %.0f K",
                      id.GetText(), radiance[0], radiance[1], radiance[2],
                      colorTemperature);
        // Through the trace as well as the render stats, as an area light's
        // report is below; a dome's reached only the stats.
        if (!entryReport.empty()) {
            HdClaudeTrace("light <%s>: %s", id.GetText(), entryReport.c_str());
        }
        *dirtyBits = HdLight::Clean;
        return;
    }

    // Scale reaches the light through its transform, so extents are measured in
    // world space rather than trusting the authored width and height alone.
    const GfVec3f xAxis(transform.TransformDir(GfVec3d(1.0, 0.0, 0.0)));
    const GfVec3f yAxis(transform.TransformDir(GfVec3d(0.0, 1.0, 0.0)));

    if (_lightType == HdPrimTypeTokens->rectLight) {
        const float width =
            Param<float>(sceneDelegate, id, HdLightTokens->width, 1.0f);
        const float height =
            Param<float>(sceneDelegate, id, HdLightTokens->height, 1.0f);
        StoreVector(light.uAxis, xAxis * (width * 0.5f));
        StoreVector(light.vAxis, yAxis * (height * 0.5f));
        light.area = 4.0f *
                     GfVec3f(light.uAxis[0], light.uAxis[1], light.uAxis[2])
                         .GetLength() *
                     GfVec3f(light.vAxis[0], light.vAxis[1], light.vAxis[2])
                         .GetLength();
        light.type = static_cast<std::uint32_t>(hdclaude::LightType::Rect);
    } else if (_lightType == HdPrimTypeTokens->diskLight) {
        const float radius =
            Param<float>(sceneDelegate, id, HdLightTokens->radius, 0.5f) *
            xAxis.GetLength();
        light.radius = radius;
        light.area = static_cast<float>(M_PI) * radius * radius;
        light.type = static_cast<std::uint32_t>(hdclaude::LightType::Disk);
    } else if (_lightType == HdPrimTypeTokens->sphereLight) {
        const float radius =
            Param<float>(sceneDelegate, id, HdLightTokens->radius, 0.5f) *
            xAxis.GetLength();
        light.radius = radius;
        light.area = 4.0f * static_cast<float>(M_PI) * radius * radius;
        light.type = static_cast<std::uint32_t>(hdclaude::LightType::Sphere);
    } else if (_lightType == HdPrimTypeTokens->cylinderLight) {
        // USD's cylinder runs along its local X, with no end caps, so the
        // lateral area is what the sampler covers and what the density uses.
        const float radius =
            Param<float>(sceneDelegate, id, HdLightTokens->radius, 0.5f) *
            yAxis.GetLength();
        const float length =
            Param<float>(sceneDelegate, id, HdLightTokens->length, 1.0f);
        StoreVector(light.uAxis, xAxis * (length * 0.5f));
        light.radius = radius;
        light.area = 2.0f * static_cast<float>(M_PI) * radius *
                     GfVec3f(light.uAxis[0], light.uAxis[1], light.uAxis[2])
                             .GetLength() * 2.0f;
        light.type = static_cast<std::uint32_t>(hdclaude::LightType::Cylinder);
    } else if (_lightType == HdPrimTypeTokens->distantLight) {
        // UsdLux authors the full angular *diameter*, in degrees.
        const float angle =
            Param<float>(sceneDelegate, id, HdLightTokens->angle, 0.53f);
        light.angularRadius =
            std::max(1.0e-4f, angle * 0.5f * static_cast<float>(M_PI) / 180.0f);
        light.type = static_cast<std::uint32_t>(hdclaude::LightType::Distant);
        if (normalize) {
            radiance /= DistantSizeFactor(angle);
        }
    } else {
        HdClaudeTrace("light <%s>: unsupported type %s", id.GetText(),
                      _lightType.GetText());
        param->SceneStore()->RemoveLight(id);
        *dirtyBits = HdLight::Clean;
        return;
    }

    // --- Shaping -------------------------------------------------------------
    // UsdLuxShapingAPI turns any of these into a spot. The cone is stored as a
    // cosine so the shader compares without a trigonometric call, and an
    // unshaped light keeps -1, which no cosine can fall below.
    const float coneAngle =
        Param<float>(sceneDelegate, id, HdLightTokens->shapingConeAngle, 180.0f);
    if (coneAngle < 180.0f) {
        light.coneCosAngle =
            std::cos(std::clamp(coneAngle, 0.0f, 180.0f) *
                     static_cast<float>(M_PI) / 180.0f);
        light.coneSoftness = std::clamp(
            Param<float>(sceneDelegate, id, HdLightTokens->shapingConeSoftness,
                         0.0f),
            0.0f, 1.0f);
    }
    light.focus = std::max(
        0.0f, Param<float>(sceneDelegate, id, HdLightTokens->shapingFocus, 0.0f));

    if (!Param<SdfAssetPath>(sceneDelegate, id, HdLightTokens->shapingIesFile,
                             SdfAssetPath())
             .GetAssetPath()
             .empty()) {
        Unhonoured(&entryReport,
                   "the IES profile is ignored; hdClaude applies only the cone "
                   "and focus terms of UsdLuxShapingAPI");
    }

    // UsdLux `normalize` makes a light's total power independent of its size,
    // so the radiance an area light emits falls as its area grows. A distant
    // light was divided by its own size factor above.
    if (normalize && light.area > 0.0f) {
        radiance /= light.area;
    }
    StoreVector(light.radiance, radiance);

    // Texture maps on area lights are not sampled: the light emits its colour
    // uniformly.
    if (_lightType == HdPrimTypeTokens->rectLight &&
        !Param<SdfAssetPath>(sceneDelegate, id, HdLightTokens->textureFile,
                             SdfAssetPath())
             .GetAssetPath()
             .empty()) {
        Unhonoured(&entryReport, "inputs:texture:file is not sampled; the rect "
                                 "light emits its colour uniformly");
    }

    // The emitter as the path tracer samples it, for the snapshot to integrate
    // what a unit of its luminance delivers.
    if (brightness.target != HdClaudeLightBrightness::Target::None && areaLight) {
        hdclaude::AreaEmitter& emitter = brightness.emitter;
        emitter.shape = _lightType == HdPrimTypeTokens->rectLight
                            ? hdclaude::EmitterShape::Rect
                        : _lightType == HdPrimTypeTokens->diskLight
                            ? hdclaude::EmitterShape::Disk
                        : _lightType == HdPrimTypeTokens->sphereLight
                            ? hdclaude::EmitterShape::Sphere
                            : hdclaude::EmitterShape::Cylinder;
        for (int i = 0; i < 3; ++i) {
            emitter.position[i] = light.position[i];
            emitter.direction[i] = light.direction[i];
            emitter.uAxis[i] = light.uAxis[i];
            emitter.vAxis[i] = light.vAxis[i];
        }
        emitter.radius = light.radius;
        emitter.shaping.coneCosAngle = light.coneCosAngle;
        emitter.shaping.coneSoftness = light.coneSoftness;
        emitter.shaping.focus = light.focus;
    }

    HdClaudeLightEntry entry;
    entry.light = light;
    entry.lightLink = lightLink;
    entry.shadowLink = shadowLink;
    entry.report = entryReport;
    entry.brightness = std::move(brightness);
    const std::size_t spectrumSamples = emissionSpectrum.wavelengths.size();
    entry.emissionSpectrum = std::move(emissionSpectrum);
    param->SceneStore()->PublishLight(id, std::move(entry));

    HdClaudeTrace(
        "light <%s>: type %u, radiance %.3f %.3f %.3f, area %.4f, shadows %s, "
        "tabulated spectrum %zu samples (luminous scale %g)",
        id.GetText(), light.type, light.radiance[0], light.radiance[1],
        light.radiance[2], light.area, light.castsShadows ? "yes" : "no",
        spectrumSamples, spectrumSamples > 0 ? light.temperatureScale : 0.0f);

    // And what this light asked for that it did not get. The report already
    // travels to `GetRenderStats`, which is where a host reads it, but that is
    // no help to anyone looking at a render from a terminal -- and this is the
    // channel every other finding about a scene comes out of.
    if (!entryReport.empty()) {
        HdClaudeTrace("light <%s>: %s", id.GetText(), entryReport.c_str());
    }

    *dirtyBits = HdLight::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
