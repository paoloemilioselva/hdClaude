#include "light.h"

#include "render_param.h"
#include "scene_store.h"
#include "texture_loader.h"
#include "trace.h"

#include "hdclaude/core/spectrum.h"

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
    const GfMatrix4d transform = sceneDelegate->GetTransform(id);
    const GfVec3f position(transform.ExtractTranslation());
    // USD lights emit along their local -Z.
    const GfVec3f emitDirection =
        GfVec3f(transform.TransformDir(GfVec3d(0.0, 0.0, -1.0))).GetNormalized();

    std::string entryReport;

    hdclaude::Light light;
    light.colorTemperature = colorTemperature;
    light.temperatureScale = temperatureScale;
    StoreVector(light.position, position);
    StoreVector(light.direction, emitDirection);
    light.castsShadows =
        Param<bool>(sceneDelegate, id, HdLightTokens->shadowEnable, true) ? 1u : 0u;

    if (_lightType == HdPrimTypeTokens->domeLight) {
        // Not an emitter in the light table: the environment a ray sees when it
        // leaves the scene, sampled by the environment strategy instead.
        HdClaudeLightEntry entry;
        entry.isDome = true;
        entry.domeColorTemperature = colorTemperature;
        entry.domeTemperatureScale = temperatureScale;
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
                entry.domeLightToWorld[column * 4 + row] =
                    static_cast<float>(transform[column][row]);
            }
        }
        param->SceneStore()->PublishLight(id, std::move(entry));
        HdClaudeTrace("dome light <%s>: environment %.3f %.3f %.3f, %.0f K",
                      id.GetText(), radiance[0], radiance[1], radiance[2],
                      colorTemperature);
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
