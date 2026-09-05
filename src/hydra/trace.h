#pragma once

// Opt-in progress tracing.
//
// A path tracer that dies mid-frame gives a host no useful message: the AOV is
// simply never written. HDCLAUDE_TRACE=1 makes each stage of a Sync and a
// render pass announce itself on stderr, unbuffered, so the last line printed
// names the stage that failed. Cheap enough to keep in the shipping build --
// the check is one cached bool and the calls do nothing when it is off.

#include "pxr/base/tf/getenv.h"

#include <algorithm>
#include <cstdarg>
#include <cstring>
#include <cstdio>

PXR_NAMESPACE_OPEN_SCOPE

inline bool HdClaudeTraceEnabled()
{
    static const bool enabled = TfGetenvBool("HDCLAUDE_TRACE", false);
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
