# ---------------------------------------------------------------------------
# hdClaudeDenoise
#
# The final-frame denoiser, Intel Open Image Denoise behind its C API, which
# denoiser.cpp loads at run time (cmake/OpenImageDenoise.cmake). No Vulkan and
# no OpenUSD: it takes images and gives one back, so it is tested without
# either. Without the OIDN headers it compiles its stubs, and every caller
# behaves the same, told why the denoiser is unavailable.
# ---------------------------------------------------------------------------

add_library(hdClaudeDenoise STATIC denoiser.cpp)
add_library(hdClaude::Denoise ALIAS hdClaudeDenoise)

target_include_directories(hdClaudeDenoise PUBLIC
    $<BUILD_INTERFACE:${CMAKE_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)

target_link_libraries(hdClaudeDenoise PUBLIC hdClaudeProjectOptions PRIVATE hdClaude::Core)
if(HDCLAUDE_ENABLE_OIDN AND HDCLAUDE_OIDN_AVAILABLE)
  target_link_libraries(hdClaudeDenoise PRIVATE hdClaudeOidnHeaders)
endif()
# dlopen, before glibc 2.34 folded libdl into libc.
target_link_libraries(hdClaudeDenoise PRIVATE ${CMAKE_DL_LIBS})

set_target_properties(hdClaudeDenoise PROPERTIES POSITION_INDEPENDENT_CODE ON)
