#include "light.h"

#include "render_param.h"
#include "scene_store.h"
#include "texture_loader.h"
#include "trace.h"

#include "pxr/base/gf/matrix4d.h"
#include "pxr/base/gf/vec3d.h"
#include "pxr/base/gf/vec3f.h"
#include "pxr/imaging/hd/sceneDelegate.h"
#include "pxr/imaging/hd/tokens.h"
#include "pxr/usd/usdLux/tokens.h"

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

/// Linear sRGB of a blackbody at `kelvin`, normalised to unit luminance.
///
/// UsdLux multiplies the light's colour by this when
/// `enableColorTemperature` is set. Krystek's rational fit for the Planckian
/// locus in CIE 1960 uv, converted to xy and then to linear sRGB: accurate to
/// well under a MacAdam step across 1667-25000 K, which is the whole range a
/// light is authored in.
///
/// Normalising to unit luminance is what makes the control a *colour* rather
/// than a brightness, so raising the temperature does not also raise exposure.
GfVec3f BlackbodyRgb(float kelvin)
{
    const float t = std::clamp(kelvin, 1667.0f, 25000.0f);
    const float t2 = t * t;
    const float t3 = t2 * t;

    const float u = (0.860117757f + 1.54118254e-4f * t + 1.28641212e-7f * t2) /
                    (1.0f + 8.42420235e-4f * t + 7.08145163e-7f * t2);
    const float v = (0.317398726f + 4.22806245e-5f * t + 4.20481691e-8f * t2) /
                    (1.0f - 2.89741816e-5f * t + 1.61456053e-7f * t2);

    const float denominator = 2.0f * u - 8.0f * v + 4.0f;
    const float x = 3.0f * u / denominator;
    const float y = 2.0f * v / denominator;
    const float z = 1.0f - x - y;
    (void)t3;

    // xyY at Y = 1 to XYZ, then the linear sRGB primaries.
    const float X = x / std::max(y, 1e-6f);
    const float Z = z / std::max(y, 1e-6f);

    GfVec3f rgb(3.2404542f * X - 1.5371385f - 0.4985314f * Z,
                -0.9692660f * X + 1.8760108f + 0.0415560f * Z,
                0.0556434f * X - 0.2040259f + 1.0572252f * Z);

    rgb[0] = std::max(rgb[0], 0.0f);
    rgb[1] = std::max(rgb[1], 0.0f);
    rgb[2] = std::max(rgb[2], 0.0f);

    const float luminance =
        0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
    if (luminance > 1e-6f) {
        rgb /= luminance;
    }
    return rgb;
}

void StoreVector(float (&out)[3], const GfVec3f& value)
{
    out[0] = value[0];
    out[1] = value[1];
    out[2] = value[2];
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
    // colour * intensity * 2^exposure, the UsdLux definition. `enableColorTemperature`
    // is deliberately not applied: blackbody conversion belongs with the
    // spectral upsampling that phase 6 brings, and applying an RGB
    // approximation now would have to be unlearned.
    const float intensity =
        Param<float>(sceneDelegate, id, HdLightTokens->intensity, 1.0f);
    const float exposure =
        Param<float>(sceneDelegate, id, HdLightTokens->exposure, 0.0f);
    const GfVec3f color =
        ParamColor(sceneDelegate, id, HdLightTokens->color, GfVec3f(1.0f));
    const bool normalize =
        Param<bool>(sceneDelegate, id, HdLightTokens->normalize, false);

    GfVec3f radiance = color * intensity * std::pow(2.0f, exposure);

    // Colour temperature tints, it does not brighten: BlackbodyRgb is
    // normalised to unit luminance, so enabling it changes the hue of a light
    // without changing how much light it emits.
    if (Param<bool>(sceneDelegate, id, HdLightTokens->enableColorTemperature,
                    false)) {
        const float kelvin = Param<float>(
            sceneDelegate, id, HdLightTokens->colorTemperature, 6500.0f);
        const GfVec3f tint = BlackbodyRgb(kelvin);
        radiance = GfVec3f(radiance[0] * tint[0], radiance[1] * tint[1],
                           radiance[2] * tint[2]);
    }

    // --- Placement -------------------------------------------------------------
    const GfMatrix4d transform = sceneDelegate->GetTransform(id);
    const GfVec3f position(transform.ExtractTranslation());
    // USD lights emit along their local -Z.
    const GfVec3f emitDirection =
        GfVec3f(transform.TransformDir(GfVec3d(0.0, 0.0, -1.0))).GetNormalized();

    std::string entryReport;

    hdclaude::Light light;
    StoreVector(light.position, position);
    StoreVector(light.direction, emitDirection);
    light.castsShadows =
        Param<bool>(sceneDelegate, id, HdLightTokens->shadowEnable, true) ? 1u : 0u;

    if (_lightType == HdPrimTypeTokens->domeLight) {
        // Not an emitter: the environment a ray sees when it leaves the scene.
        // A textured dome is not applied -- hdClaude binds no textures -- so
        // only the constant colour contributes, and the difference is reported
        // rather than passed off as the authored environment.
        HdClaudeLightEntry entry;
        entry.isDome = true;
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

        // The dome's own rotation, inverted: the environment kernel takes a
        // world direction into the map's frame, not the other way round.
        const GfMatrix4d worldToLight = transform.GetInverse();
        for (int column = 0; column < 4; ++column) {
            for (int row = 0; row < 4; ++row) {
                entry.domeWorldToLight[column * 4 + row] =
                    static_cast<float>(worldToLight[column][row]);
            }
        }
        param->SceneStore()->PublishLight(id, std::move(entry));
        HdClaudeTrace("dome light <%s>: environment %.3f %.3f %.3f", id.GetText(),
                      radiance[0], radiance[1], radiance[2]);
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
        entryReport =
            "the IES profile is ignored; hdClaude applies only the cone and "
            "focus terms of UsdLuxShapingAPI";
    }

    // UsdLux `normalize` makes a light's total power independent of its size,
    // so the radiance an area light emits falls as its area grows. A distant
    // light has no area and is unaffected.
    if (normalize && light.area > 0.0f) {
        radiance /= light.area;
    }
    StoreVector(light.radiance, radiance);

    HdClaudeLightEntry entry;
    entry.light = light;
    entry.report = std::move(entryReport);
    param->SceneStore()->PublishLight(id, std::move(entry));

    HdClaudeTrace(
        "light <%s>: type %u, radiance %.3f %.3f %.3f, area %.4f, shadows %s",
        id.GetText(), light.type, light.radiance[0], light.radiance[1],
        light.radiance[2], light.area, light.castsShadows ? "yes" : "no");

    *dirtyBits = HdLight::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
