// Karma's rendervisibility pattern. See ray_visibility.h.
#include "hdclaude/core/ray_visibility.h"

#include <algorithm>

namespace hdclaude {

namespace {

// Karma's five kinds, each a bit of its own while the pattern is read; the
// three indirect ones collapse into kRayIndirect afterwards.
constexpr std::uint8_t kPrimary = 0x01;
constexpr std::uint8_t kShadow = 0x02;
constexpr std::uint8_t kDiffuse = 0x04;
constexpr std::uint8_t kReflect = 0x08;
constexpr std::uint8_t kRefract = 0x10;
constexpr std::uint8_t kIndirectKinds = kDiffuse | kReflect | kRefract;
constexpr std::uint8_t kEveryKind = kPrimary | kShadow | kIndirectKinds;

std::uint8_t KindOf(std::string_view name)
{
    if (name == "*") {
        return kEveryKind;
    }
    if (name == "primary") {
        return kPrimary;
    }
    if (name == "shadow") {
        return kShadow;
    }
    if (name == "diffuse") {
        return kDiffuse;
    }
    if (name == "reflect") {
        return kReflect;
    }
    if (name == "refract") {
        return kRefract;
    }
    return 0;
}

void Report(KarmaRenderVisibility& result, std::string line)
{
    if (std::find(result.reports.begin(), result.reports.end(), line) ==
        result.reports.end()) {
        result.reports.push_back(std::move(line));
    }
}

}  // namespace

KarmaRenderVisibility ParseKarmaRenderVisibility(std::string_view pattern)
{
    KarmaRenderVisibility result;
    std::uint8_t kinds = 0;
    bool first = true;
    std::size_t i = 0;
    while (i < pattern.size()) {
        const char c = pattern[i];
        if (c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == ',') {
            ++i;
            continue;
        }
        std::size_t end = i;
        while (end < pattern.size() && pattern[end] != ' ' &&
               pattern[end] != '\t' && pattern[end] != '\n' &&
               pattern[end] != '\r' && pattern[end] != ',') {
            ++end;
        }
        std::string_view term = pattern.substr(i, end - i);
        i = end;

        bool remove = false;
        if (term.front() == '-' || term.front() == '^') {
            remove = true;
            term.remove_prefix(1);
        } else if (term.front() == '+') {
            term.remove_prefix(1);
        }
        if (first && remove) {
            kinds = kEveryKind;
        }
        first = false;

        const std::uint8_t kind = KindOf(term);
        if (kind == 0) {
            Report(result, "rendervisibility names '" + std::string(term) +
                               "', which is not a kind of ray; ignored");
            continue;
        }
        kinds = remove ? static_cast<std::uint8_t>(kinds & ~kind)
                       : static_cast<std::uint8_t>(kinds | kind);
    }

    if (first) {
        // Nothing but separators: no kind named at all.
        Report(result, "rendervisibility is empty; read as '*'");
        return result;
    }

    const std::uint8_t indirect = kinds & kIndirectKinds;
    if (indirect != 0 && indirect != kIndirectKinds) {
        Report(result,
               "rendervisibility separates 'diffuse', 'reflect' and 'refract', "
               "which hdClaude does not yet tell apart; read as visible to "
               "every indirect ray");
    }
    result.visibility = static_cast<std::uint8_t>(
        ((kinds & kPrimary) ? kRayCamera : 0) |
        ((kinds & kShadow) ? kRayShadow : 0) | (indirect ? kRayIndirect : 0));
    return result;
}

}  // namespace hdclaude
