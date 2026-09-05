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
