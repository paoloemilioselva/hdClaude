// VulkanMemoryAllocator implementation unit.
//
// VMA is a single-header library; this translation unit is where its
// implementation is instantiated. It is kept alone in its own file because the
// implementation macro must appear exactly once, and because VMA's headers
// generate warnings that hdClaude's own warning level would otherwise reject.

#if defined(_MSC_VER)
#pragma warning(push, 0)
#endif

#define VMA_IMPLEMENTATION
#define VMA_STATIC_VULKAN_FUNCTIONS 0
#define VMA_DYNAMIC_VULKAN_FUNCTIONS 1
#include <volk.h>
#include <vk_mem_alloc.h>

#if defined(_MSC_VER)
#pragma warning(pop)
#endif
