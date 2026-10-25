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
  # The kernels are compiled from the source tree, not the install, so a kernel
  # that does not compile fails here rather than at the first render.
  target_compile_definitions(hdClaudeGlslCompilerTests PRIVATE
      HDCLAUDE_SHADER_DIRECTORY="${CMAKE_SOURCE_DIR}/shaders")
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
  # Off Windows the manifest names a bare library the loader finds on the
  # library search path, so the layer's lib directory travels with the tests.
  set(_layer_path_modification "")
  if(HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR)
    set(_layer_path_modification
        "LD_LIBRARY_PATH=path_list_prepend:${HDCLAUDE_VALIDATION_LAYER_LIBRARY_DIR}")
  endif()
  set_tests_properties(gpu geometry PROPERTIES
      SKIP_RETURN_CODE 77
      ENVIRONMENT "${_gpu_test_env}"
      ENVIRONMENT_MODIFICATION "${_layer_path_modification}")

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
  if(WIN32)
    set(_usd_path_modification
        "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/bin"
        "PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/lib")
  else()
    set(_usd_path_modification
        "LD_LIBRARY_PATH=path_list_prepend:${HDCLAUDE_OPENUSD_ROOT}/lib")
  endif()
  list(APPEND _usd_path_modification ${_layer_path_modification})

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
      HDCLAUDE_VALIDATION_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/closure_validation.comp.glsl"
      HDCLAUDE_CHI2_KERNEL="${CMAKE_CURRENT_SOURCE_DIR}/closure_chi2.comp.glsl")
  add_test(NAME closure_validation COMMAND hdClaudeClosureValidationTests)
  set_tests_properties(closure_validation PROPERTIES SKIP_RETURN_CODE 77)

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
      SKIP_RETURN_CODE 77
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeRenderTests hdClaudeValidationLayers)
  endif()

  # The Hydra adapter classes that stand alone -- a render buffer needs only an
  # id, an instancer only a scene delegate -- compiled from the plugin's own
  # sources, since nothing links against the plugin module. Tests the source
  # tree rather than the install.
  if(HDCLAUDE_BUILD_HYDRA_PLUGIN)
    add_executable(hdClaudeHydraTests
        hydra_tests.cpp
        ${CMAKE_SOURCE_DIR}/src/hydra/instancer.cpp
        ${CMAKE_SOURCE_DIR}/src/hydra/render_buffer.cpp)
    target_include_directories(hdClaudeHydraTests PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}
        ${CMAKE_SOURCE_DIR}/src/hydra
        ${CMAKE_SOURCE_DIR}/include)
    target_link_libraries(hdClaudeHydraTests PRIVATE
        hdClaudeProjectOptions hdClaude::Core gf hd sdf tf vt)
    if(MSVC)
      # The plugin's own suppressions, for the same OpenUSD headers.
      target_compile_options(hdClaudeHydraTests PRIVATE
          /wd4244 /wd4267 /wd4275 /wd4251)
    endif()
    add_test(NAME hydra COMMAND hdClaudeHydraTests)
    set_tests_properties(hydra PROPERTIES
        ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  endif()

  # Phase 3 and 8: instanced geometry renders where USD places it. The
  # scripted tests that reach the Hydra adapters, so each runs a script in the OpenUSD
  # environment and renders through the installed plugin -- build and install
  # (compile.py) before trusting it. Each script runs through the platform's
  # wrapper, which sources setup_usd_env first.
  if(TARGET hdClaudeImageDiff)
    foreach(_usd_test
            "usd_instancing|instancing_equivalence"
            "usd_lights|light_linking_equivalence"
            "usd_cull_volumes|cull_volume_equivalence"
            "usd_implicit_surfaces|implicit_surfaces"
            "usd_texture_quality|texture_quality"
            "usd_picking|picking"
            "usd_depth|depth"
            "usd_houdini_parameters|houdini_parameters")
      string(REPLACE "|" ";" _usd_test "${_usd_test}")
      list(GET _usd_test 0 _name)
      list(GET _usd_test 1 _script)
      if(WIN32)
        set(_command "${CMAKE_CURRENT_SOURCE_DIR}/usd/run_${_script}.bat")
      else()
        set(_command bash "${CMAKE_CURRENT_SOURCE_DIR}/usd/run_in_usd_env.sh"
                          "${_script}.py")
      endif()
      add_test(NAME ${_name}
          COMMAND ${_command} "${CMAKE_CURRENT_BINARY_DIR}/${_name}")
      set_tests_properties(${_name} PROPERTIES
          ENVIRONMENT "HDCLAUDE_IMAGE_DIFF=$<TARGET_FILE:hdClaudeImageDiff>")
    endforeach()
  endif()

  set_tests_properties(closure_validation PROPERTIES
      ENVIRONMENT "${_validation_env}"
      ENVIRONMENT_MODIFICATION "${_usd_path_modification}")
  if(TARGET hdClaudeValidationLayers)
    add_dependencies(hdClaudeClosureValidationTests hdClaudeValidationLayers)
  endif()
endif()
