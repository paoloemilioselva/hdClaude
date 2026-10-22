#pragma once

// Export macro for the cull volume imaging library.

#if defined(_WIN32)
#  if defined(USDIMAGINGCULLVOLUME_EXPORTS)
#    define USDIMAGINGCULLVOLUME_API __declspec(dllexport)
#  else
#    define USDIMAGINGCULLVOLUME_API __declspec(dllimport)
#  endif
#else
#  define USDIMAGINGCULLVOLUME_API __attribute__((visibility("default")))
#endif
