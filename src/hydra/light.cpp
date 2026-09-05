#include "light.h"

#include "render_param.h"
#include "scene_store.h"
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

    // --- Placement -------------------------------------------------------------
    const GfMatrix4d transform = sceneDelegate->GetTransform(id);
    const GfVec3f position(transform.ExtractTranslation());
    // USD lights emit along their local -Z.
    const GfVec3f emitDirection =
        GfVec3f(transform.TransformDir(GfVec3d(0.0, 0.0, -1.0))).GetNormalized();

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
        if (!Param<SdfAssetPath>(sceneDelegate, id, HdLightTokens->textureFile,
                                 SdfAssetPath())
                 .GetAssetPath()
                 .empty()) {
            entry.report =
                "the dome light's texture is ignored; hdClaude does not bind "
                "textures yet, so only its constant colour lights the scene";
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

    // UsdLux `normalize` makes a light's total power independent of its size,
    // so the radiance an area light emits falls as its area grows. A distant
    // light has no area and is unaffected.
    if (normalize && light.area > 0.0f) {
        radiance /= light.area;
    }
    StoreVector(light.radiance, radiance);

    HdClaudeLightEntry entry;
    entry.light = light;
    param->SceneStore()->PublishLight(id, std::move(entry));

    HdClaudeTrace(
        "light <%s>: type %u, radiance %.3f %.3f %.3f, area %.4f, shadows %s",
        id.GetText(), light.type, light.radiance[0], light.radiance[1],
        light.radiance[2], light.area, light.castsShadows ? "yes" : "no");

    *dirtyBits = HdLight::Clean;
}

PXR_NAMESPACE_CLOSE_SCOPE
