// Per-ray-kind visibility of an rprim. See ray_visibility.h.
#include "ray_visibility.h"

#include "trace.h"

#include "hdclaude/core/ray_visibility.h"

#include "pxr/base/tf/diagnostic.h"
#include "pxr/base/tf/staticTokens.h"
#include "pxr/base/vt/array.h"
#include "pxr/base/vt/types.h"
#include "pxr/base/vt/value.h"
#include "pxr/imaging/hd/sceneDelegate.h"

#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <utility>

PXR_NAMESPACE_OPEN_SCOPE

namespace {

TF_DEFINE_PRIVATE_TOKENS(
    _tokens,
    ((riCamera, "ri:attributes:visibility:camera"))
    ((riIndirect, "ri:attributes:visibility:indirect"))
    ((riTransmission, "ri:attributes:visibility:transmission"))
    ((karmaRenderVisibility, "karma:object:rendervisibility"))
);

/// Once per prim and message: a stage is synced again on every edit, and the
/// same unreadable value would otherwise be warned about each time.
void WarnOnce(const SdfPath& id, const std::string& message)
{
    static std::mutex mutex;
    static std::set<std::pair<SdfPath, std::string>> warned;
    {
        std::lock_guard<std::mutex> lock(mutex);
        if (!warned.emplace(id, message).second) {
            return;
        }
    }
    TF_WARN("hdClaude: <%s>: %s.", id.GetText(), message.c_str());
}

/// A one-element array, as some exporters author a constant primvar, is read
/// as its element.
template <class T>
std::optional<T> Single(const VtValue& value)
{
    if (value.IsHolding<T>()) {
        return value.UncheckedGet<T>();
    }
    if (value.IsHolding<VtArray<T>>()) {
        const VtArray<T>& array = value.UncheckedGet<VtArray<T>>();
        if (array.size() == 1) {
            return array[0];
        }
    }
    return std::nullopt;
}

/// RenderMan's int switch: zero hides, anything else shows.
std::optional<bool> ReadSwitch(const VtValue& value)
{
    if (auto v = Single<int>(value)) {
        return *v != 0;
    }
    if (auto v = Single<bool>(value)) {
        return *v;
    }
    if (auto v = Single<int64_t>(value)) {
        return *v != 0;
    }
    if (auto v = Single<unsigned int>(value)) {
        return *v != 0u;
    }
    if (auto v = Single<float>(value)) {
        return *v != 0.0f;
    }
    if (auto v = Single<double>(value)) {
        return *v != 0.0;
    }
    return std::nullopt;
}

std::optional<std::string> ReadString(const VtValue& value)
{
    if (auto v = Single<std::string>(value)) {
        return *v;
    }
    if (auto v = Single<TfToken>(value)) {
        return v->GetString();
    }
    return std::nullopt;
}

}  // namespace

std::uint8_t HdClaudeRayVisibility(HdSceneDelegate* delegate, const SdfPath& id)
{
    std::uint8_t visibility = hdclaude::kRayAll;
    if (delegate == nullptr) {
        return visibility;
    }
    for (const HdPrimvarDescriptor& descriptor :
         delegate->GetPrimvarDescriptors(id, HdInterpolationConstant)) {
        const TfToken& name = descriptor.name;
        std::uint8_t kind = 0;
        if (name == _tokens->riCamera) {
            kind = hdclaude::kRayCamera;
        } else if (name == _tokens->riIndirect) {
            kind = hdclaude::kRayIndirect;
        } else if (name == _tokens->riTransmission) {
            kind = hdclaude::kRayShadow;
        } else if (name != _tokens->karmaRenderVisibility) {
            continue;
        }

        const VtValue value = delegate->Get(id, name);
        if (value.IsEmpty()) {
            continue;
        }
        if (kind != 0) {
            const std::optional<bool> shown = ReadSwitch(value);
            if (!shown) {
                WarnOnce(id, "primvar '" + name.GetString() + "' holds a " +
                                 value.GetTypeName() +
                                 ", not an int; ignored");
            } else if (!*shown) {
                visibility = static_cast<std::uint8_t>(visibility & ~kind);
            }
            continue;
        }

        const std::optional<std::string> pattern = ReadString(value);
        if (!pattern) {
            WarnOnce(id, "primvar '" + name.GetString() + "' holds a " +
                             value.GetTypeName() + ", not a string; ignored");
            continue;
        }
        const hdclaude::KarmaRenderVisibility karma =
            hdclaude::ParseKarmaRenderVisibility(*pattern);
        for (const std::string& report : karma.reports) {
            WarnOnce(id, report + " (\"" + *pattern + "\")");
        }
        visibility = static_cast<std::uint8_t>(visibility & karma.visibility);
    }

    if (visibility != hdclaude::kRayAll) {
        HdClaudeTrace("rprim <%s>: hidden from%s%s%s rays", id.GetText(),
                      (visibility & hdclaude::kRayCamera) ? "" : " camera",
                      (visibility & hdclaude::kRayIndirect) ? "" : " indirect",
                      (visibility & hdclaude::kRayShadow) ? "" : " shadow");
    }
    return visibility;
}

PXR_NAMESPACE_CLOSE_SCOPE
