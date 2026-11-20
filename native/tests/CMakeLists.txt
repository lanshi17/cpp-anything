# Golden parity: replay tests/golden/cli.jsonl through the C++ binary. Each case
# records argv, stdout, stderr and the exit code, exactly as `rsi record` wrote it.
find_package(Python3 REQUIRED COMPONENTS Interpreter)

add_test(
  NAME golden-cli
  COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/golden.py
          $<TARGET_FILE:cpp-anything>
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/golden/cli.jsonl
  WORKING_DIRECTORY ${CMAKE_CURRENT_SOURCE_DIR}/../..
)
