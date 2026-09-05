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

  # Geometry and traversal. Scenes are built programmatically, never read from
  # USD: the GPU backend has no USD dependency and geometry reaches it only
  # through the Scene struct the Hydra mesh adapter fills.
  add_executable(hdClaudeGeometryTests geometry_tests.cpp)
  target_link_libraries(hdClaudeGeometryTests PRIVATE hdClaude::Gpu)
  target_include_directories(hdClaudeGeometryTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(hdClaudeGeometryTests PRIVATE
      HDCLAUDE_TRAVERSAL_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/traversal.comp.glsl")
  add_test(NAME geometry COMMAND hdClaudeGeometryTests)

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
  set_tests_properties(geometry PROPERTIES ENVIRONMENT "${_gpu_test_env}")

  # The layer must exist before the test runs, so the test binary depends on
  # the external build rather than relying on build order by accident.
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeGpuTests hdClaudeValidationLayers)
    add_dependencies(hdClaudeGeometryTests hdClaudeValidationLayers)
  endif()
endif()

if(HDCLAUDE_ENABLE_MATERIALX)
  # MaterialX-linked tests load OpenUSD's DLLs at runtime. ctest does not
  # inherit whatever PATH a developer happened to set, so the tests carry it
  # themselves -- otherwise `ctest` fails with 0xC0000135 (DLL not found) while
  # running the same binary by hand succeeds, which is a confusing way to
  # discover a missing search path.
  # Runtime search path for OpenUSD's DLLs, and the validation environment.
  # Both are defined before any test uses them: a CMake variable read before it
  # is set expands to nothing and the test silently runs unconfigured.
  set(_usd_path_modification
      "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/bin"
      "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/lib")

  set(_validation_env "HDCLAUDE_ENABLE_VULKAN_VALIDATION=1")
  if(HDCLAUDE_VALIDATION_LAYER_DIR)
    list(APPEND _validation_env "VK_LAYER_PATH=${HDCLAUDE_VALIDATION_LAYER_DIR}")
  endif()
  # Generates a material through genglsl_pt and compiles it to SPIR-V. Needs the
  # MaterialX libraries from the OpenUSD distribution, but no GPU and no stage.
  add_executable(hdClaudeMaterialXTests materialx_tests.cpp)
  target_link_libraries(hdClaudeMaterialXTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeMaterialXTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME materialx COMMAND hdClaudeMaterialXTests)
  set_tests_properties(materialx PROPERTIES
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")

  # The phase 4 acceptance gate: generate each closure, compile it with a
  # validation kernel, and run it on the GPU. Asks whether the shading is
  # *right*, where every other test asks whether it compiles.
  add_executable(hdClaudeClosureValidationTests closure_validation_tests.cpp)
  target_link_libraries(hdClaudeClosureValidationTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeClosureValidationTests
      PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  target_compile_definitions(hdClaudeClosureValidationTests PRIVATE
      HDCLAUDE_VALIDATION_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/closure_validation.comp.glsl")
  add_test(NAME closure_validation COMMAND hdClaudeClosureValidationTests)

  # The first images: a MaterialX material shading real geometry through the
  # wavefront integrator. Asserts light transport -- lit brighter than unlit, a
  # shadow darker than its surroundings, a red surface red -- rather than pixel
  # values, because those hold for any correct renderer and none of them holds
  # for one that is merely producing output.
  add_executable(hdClaudeRenderTests render_tests.cpp)
  target_link_libraries(hdClaudeRenderTests PRIVATE hdClaude::MaterialX)
  target_include_directories(hdClaudeRenderTests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
  add_test(NAME render COMMAND hdClaudeRenderTests)
  set_tests_properties(render PROPERTIES
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeRenderTests hdClaudeValidationLayers)
  endif()

  set_tests_properties(closure_validation PROPERTIES
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeClosureValidationTests hdClaudeValidationLayers)
  endif()
endif()
