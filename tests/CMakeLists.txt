# ---------------------------------------------------------------------------
# Tests
#
# Core tests require neither a GPU nor an OpenUSD runtime and always build.
# GPU and Hydra tests are added alongside their subsystems as those land.
# ---------------------------------------------------------------------------

add_executable(hdClaudeCoreTests core_tests.cpp)
target_link_libraries(hdClaudeCoreTests PRIVATE hdClaude::Core)
target_include_directories(hdClaudeCoreTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
add_test(NAME core COMMAND hdClaudeCoreTests)

if(HDCLAUDE_ENABLE_VULKAN)
  add_executable(hdClaudeGpuTests gpu_tests.cpp)
  target_link_libraries(hdClaudeGpuTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu COMMAND hdClaudeGpuTests)

  # Validation is a gate, not a console message (docs/lessons-from-hdcodex.md
  # R8). The test binary enables the layer itself and fails on a nonzero error
  # count; this makes that explicit for anyone reading the test definition.
  set_tests_properties(gpu PROPERTIES
      ENVIRONMENT "HDCLAUDE_ENABLE_VULKAN_VALIDATION=1")
endif()
