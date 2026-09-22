#pragma once

// One reading of an environment variable, for every layer of hdClaude.
//
// There were three, and they disagreed about the same variable. `HDCLAUDE_TRACE`
// was read as `TfGetenvBool` in the Hydra layer, as `value[0] == '1'` in the
// Vulkan context, and as "is the variable set at all" in the acceleration
// structure and the path tracer -- so `HDCLAUDE_TRACE=0` switched two of the
// three *on*, and a value of `"1 "` switched one of the three *off*.
//
// The trailing space is not hypothetical: `set HDCLAUDE_TRACE=1 && program` in
// cmd assigns everything up to the `&&`, space included. That silently produced
// a half-traced render -- the GPU stages announcing themselves and every light
// and material staying quiet -- which reads exactly like a renderer that never
// synced its lights, and cost an hour of looking for a defect that was not
// there.
//
// This layer has no dependencies and cannot reach for `TfGetenv`, which is also
// why the rule lives here rather than in the Hydra layer: the lowest layer that
// needs it is the one that has to own it.

#include <algorithm>
#include <cctype>
#include <cstdlib>
#include <map>
#include <mutex>
#include <string>

namespace hdclaude {

/// Values a host has supplied that stand in front of the process environment.
///
/// Every `HDCLAUDE_*` variable is also a Hydra render setting, because Paolo
/// drives this renderer from a viewport as much as from a shell and a knob that
/// exists in only one of the two is a knob he has to restart a process to turn.
/// The settings are read by the Hydra layer, which is above this one and cannot
/// be called from it -- so the delegate pushes what it resolved down here, and
/// the layers that read the environment see the host's answer instead.
///
/// Guarded because the delegate writes these once during `Initialize` while
/// materials and geometry are synced in parallel afterwards. Reads are rare in
/// practice: every caller caches.
inline std::mutex& EnvironmentOverrideMutex()
{
    static std::mutex mutex;
    return mutex;
}

inline std::map<std::string, std::string>& EnvironmentOverrides()
{
    static std::map<std::string, std::string> overrides;
    return overrides;
}

/// Make `name` read as `value` everywhere below the Hydra layer.
///
/// An empty value *removes* the override rather than setting the variable to
/// nothing, so a host that leaves a setting at its default gets the process
/// environment back rather than a forced blank.
inline void SetEnvironmentOverride(const std::string& name,
                                   const std::string& value)
{
    std::lock_guard<std::mutex> lock(EnvironmentOverrideMutex());
    if (value.empty()) {
        EnvironmentOverrides().erase(name);
    } else {
        EnvironmentOverrides()[name] = value;
    }
}

/// An environment variable's value with surrounding whitespace removed, or
/// empty when it is unset.
inline std::string EnvironmentValue(const char* name)
{
    {
        std::lock_guard<std::mutex> lock(EnvironmentOverrideMutex());
        const auto found = EnvironmentOverrides().find(name);
        if (found != EnvironmentOverrides().end()) {
            return found->second;
        }
    }

    std::string text;
#if defined(_MSC_VER)
    char* value = nullptr;
    std::size_t size = 0;
    if (_dupenv_s(&value, &size, name) == 0 && value != nullptr) {
        text.assign(value);
        std::free(value);
    }
#else
    if (const char* value = std::getenv(name)) {
        text.assign(value);
    }
#endif
    const auto space = [](unsigned char c) { return std::isspace(c) != 0; };
    text.erase(text.begin(),
               std::find_if_not(text.begin(), text.end(), space));
    text.erase(std::find_if_not(text.rbegin(), text.rend(), space).base(),
               text.end());
    return text;
}

/// Whether a variable is set to something that means yes.
///
/// `1`, `true`, `yes` and `on` are true and `0`, `false`, `no` and `off` are
/// false, each in any case and with any surrounding whitespace. An unset
/// variable takes the default. So does a value that is neither -- because a
/// variable set to something unrecognised is a question hdClaude cannot answer,
/// and guessing at it is how `HDCLAUDE_TRACE=0` came to mean "trace".
inline bool EnvironmentFlag(const char* name, bool fallback = false)
{
    std::string text = EnvironmentValue(name);
    if (text.empty()) {
        return fallback;
    }
    std::transform(text.begin(), text.end(), text.begin(),
                   [](unsigned char c) {
                       return static_cast<char>(std::tolower(c));
                   });
    if (text == "1" || text == "true" || text == "yes" || text == "on") {
        return true;
    }
    if (text == "0" || text == "false" || text == "no" || text == "off") {
        return false;
    }
    return fallback;
}

}  // namespace hdclaude
