# Golden parity: replay tests/golden/cli.jsonl through the C++ binary. Each case
# records argv, stdout, stderr and the exit code, exactly as `rsi record` wrote it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME golden-cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py
          $<TARGET_FILE:cpp-anything>
          ${CMAKE_CURRENT_SOURCE_DIR}/golden/cli.jsonl
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/..
)

add_executable(cppan_unit_tests unit.cpp)
target_link_libraries(cppan_unit_tests PRIVATE cppan_core)
add_test(NAME unit COMMAND cppan_unit_tests)
