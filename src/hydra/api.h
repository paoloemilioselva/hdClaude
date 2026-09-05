#pragma once

// Export macro for the Hydra-discoverable classes.
//
// Only the plugin entry points and the classes Hydra instantiates need to be
// visible; everything else in this library is internal.

#if defined(_WIN32)
#  if defined(HDCLAUDE_EXPORTS)
#    define HDCLAUDE_API __declspec(dllexport)
#  else
#    define HDCLAUDE_API __declspec(dllimport)
#  endif
#else
#  define HDCLAUDE_API __attribute__((visibility("default")))
#endif
