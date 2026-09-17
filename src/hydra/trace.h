#pragma once

// Opt-in progress tracing.
//
// A path tracer that dies mid-frame gives a host no useful message: the AOV is
// simply never written. HDCLAUDE_TRACE=1 makes each stage of a Sync and a
// render pass announce itself on stderr, unbuffered, so the last line printed
// names the stage that failed. Cheap enough to keep in the shipping build --
// the check is one cached bool and the calls do nothing when it is off.

#include "hdclaude/core/environment.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

/// Read the way every other layer reads it, rather than through `TfGetenvBool`.
///
/// The two disagreed, and the disagreement was invisible: `TfGetenvBool` matches
/// its words exactly, so a value of `"1 "` -- which is what cmd assigns for
/// `set HDCLAUDE_TRACE=1 && program`, trailing space and all -- left this
/// silent while the GPU layer, which looked only at the first character, traced
/// normally. A half-traced render is worse than an untraced one: the Vulkan
/// stages announce themselves, every light and material stays quiet, and it
/// reads exactly like a renderer that never synced its lights.
inline bool HdClaudeTraceEnabled()
{
    static const bool enabled = hdclaude::EnvironmentFlag("HDCLAUDE_TRACE");
    return enabled;
}

inline void HdClaudeTrace(const char* format, ...)
{
    if (!HdClaudeTraceEnabled()) {
        return;
    }
    // Composed into one buffer and written once. Hydra syncs prims in
    // parallel, and three separate writes per message interleave into
    // unreadable output exactly when the trace is most needed.
    char message[1024];
    const int prefix = std::snprintf(message, sizeof(message), "[hdClaude] ");

    va_list arguments;
    va_start(arguments, format);
    std::vsnprintf(message + prefix, sizeof(message) - prefix - 2, format,
                   arguments);
    va_end(arguments);

    const std::size_t length = std::strlen(message);
    message[length] = '\n';
    message[length + 1] = '\0';

    std::fputs(message, stderr);
    std::fflush(stderr);
}

PXR_NAMESPACE_CLOSE_SCOPE
