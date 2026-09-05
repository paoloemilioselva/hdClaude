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
  # Compilation needs glslang but neither a GPU nor OpenUSD, so a material's
  # generated code can be checked for compilability on any machine.
  add_executable(hdClaudeGlslCompilerTests glsl_compiler_tests.cpp)
  target_link_libraries(hdClaudeGlslCompilerTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGlslCompilerTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME glsl_compiler COMMAND hdClaudeGlslCompilerTests)

  add_executable(hdClaudeGpuTests gpu_tests.cpp)
  target_link_libraries(hdClaudeGpuTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGpuTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME gpu COMMAND hdClaudeGpuTests)

  # Validation is a gate, not a console message (docs/lessons-from-hdcodex.md
  # R8). The test binary enables the layer and fails both on a nonzero error
  # count and when the layer is unavailable, so the environment is pointed at
  # whichever layer the configure located or built.
  set(_gpu_test_env "HDCLAUDE_ENABLE_VULKAN_VALIDATION=1")
  if(HDCLAUDE_VALIDATION_LAYER_DIR)
    list(APPEND _gpu_test_env "VK_LAYER_PATH=${HDCLAUDE_VALIDATION_LAYER_DIR}")
  endif()
  set_tests_properties(gpu PROPERTIES ENVIRONMENT "${_gpu_test_env}")

  # The layer must exist before the test runs, so the test binary depends on
  # the external build rather than relying on build order by accident.
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeGpuTests hdClaudeValidationLayers)
  endif()
endif()

if(HDCLAUDE_ENABLE_MATERIALX)
  # Generates a material through genglsl_pt and compiles it to SPIR-V. Needs the
  # MaterialX libraries from the OpenUSD distribution, but no GPU and no stage.
  add_executable(hdClaudeMaterialXTests materialx_tests.cpp)
  target_link_libraries(hdClaudeMaterialXTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeMaterialXTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME materialx COMMAND hdClaudeMaterialXTests)
endif()
