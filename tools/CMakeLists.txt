# ---------------------------------------------------------------------------
# Gallery tools
#
# Two small executables the gallery script drives. They read and write images
# through Hio, which the delegate already depends on, so the gallery pipeline
# introduces no image library of its own and no second opinion about what an
# EXR contains.
#
# Built only when OpenUSD is in the build, so the core-only preset -- which
# exists to be buildable with neither a GPU nor a USD runtime -- still
# configures.
# ---------------------------------------------------------------------------

if(NOT HDCLAUDE_BUILD_HYDRA_PLUGIN)
  return()
endif()

add_executable(hdClaudeDisplayTransform display_transform.cpp)
target_link_libraries(hdClaudeDisplayTransform
    PRIVATE hdClaudeProjectOptions hdClaude::Core hio)

add_executable(hdClaudeImageDiff image_diff.cpp)
target_link_libraries(hdClaudeImageDiff PRIVATE hdClaudeProjectOptions gf hio)

# OpenUSD's headers are not warning-clean at /W4; the same narrowing the
# delegate applies, for the same reason.
if(MSVC)
  target_compile_options(hdClaudeDisplayTransform PRIVATE /wd4100 /wd4201 /wd4244 /wd4267)
  target_compile_options(hdClaudeImageDiff PRIVATE /wd4100 /wd4201 /wd4244 /wd4267)
endif()

install(TARGETS hdClaudeDisplayTransform hdClaudeImageDiff
    RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})
