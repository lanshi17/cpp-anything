set(XSV_UNIT_SOURCES
  main.cpp
  test_config.cpp
  test_csv.cpp
  test_format.cpp
  test_hash.cpp
  test_index.cpp
  test_memory.cpp
  test_parallel.cpp
  test_regex.cpp
  test_select.cpp
  test_simd.cpp)

add_executable(xsv_unit_tests ${XSV_UNIT_SOURCES})
target_link_libraries(xsv_unit_tests PRIVATE xsv_core)
xsv_apply_warnings(xsv_unit_tests)
xsv_apply_sanitizers(xsv_unit_tests)

add_test(NAME unit COMMAND xsv_unit_tests)
set_tests_properties(unit PROPERTIES LABELS "unit")

# CLI smoke checks run the real binary over the fixtures.
add_test(NAME cli_count COMMAND xsv count
         ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_count PROPERTIES
  PASS_REGULAR_EXPRESSION "^3" LABELS "cli")

add_test(NAME cli_count_no_headers COMMAND xsv count -n
         ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_count_no_headers PROPERTIES
  PASS_REGULAR_EXPRESSION "^4" LABELS "cli")

add_test(NAME cli_count_quoted COMMAND xsv count
         ${CMAKE_CURRENT_SOURCE_DIR}/data/quoted.csv)
set_tests_properties(cli_count_quoted PROPERTIES
  PASS_REGULAR_EXPRESSION "^2" LABELS "cli")

add_test(NAME cli_count_crlf COMMAND xsv count
         ${CMAKE_CURRENT_SOURCE_DIR}/data/crlf.csv)
set_tests_properties(cli_count_crlf PROPERTIES
  PASS_REGULAR_EXPRESSION "^2" LABELS "cli")

add_test(NAME cli_count_ragged_fails COMMAND xsv count
         ${CMAKE_CURRENT_SOURCE_DIR}/data/ragged.csv)
set_tests_properties(cli_count_ragged_fails PROPERTIES
  WILL_FAIL TRUE LABELS "cli")

add_test(NAME cli_headers COMMAND xsv headers ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_headers PROPERTIES
  PASS_REGULAR_EXPRESSION "1   name" LABELS "cli")

add_test(NAME cli_select COMMAND xsv select 2 ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_select PROPERTIES
  PASS_REGULAR_EXPRESSION "^age" LABELS "cli")

add_test(NAME cli_slice COMMAND xsv slice -s 1 -l 1 ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_slice PROPERTIES
  PASS_REGULAR_EXPRESSION "Bob,25,London" LABELS "cli")

add_test(NAME cli_search COMMAND xsv search Bob ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_search PROPERTIES
  PASS_REGULAR_EXPRESSION "Bob,25,London" LABELS "cli")

add_test(NAME cli_sort COMMAND xsv sort -s 1 ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_sort PROPERTIES
  PASS_REGULAR_EXPRESSION "^name,age,city" LABELS "cli")

add_test(NAME cli_stats COMMAND xsv stats ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_stats PROPERTIES
  PASS_REGULAR_EXPRESSION "^field,type" LABELS "cli")

add_test(NAME cli_join COMMAND xsv join 1
         ${CMAKE_CURRENT_SOURCE_DIR}/../evaluation/correctness/fixtures/join_left.csv
         1
         ${CMAKE_CURRENT_SOURCE_DIR}/../evaluation/correctness/fixtures/join_right.csv)
set_tests_properties(cli_join PROPERTIES
  PASS_REGULAR_EXPRESSION "k,l,k,r" LABELS "cli")

add_test(NAME cli_index COMMAND xsv index -o ${CMAKE_CURRENT_BINARY_DIR}/basic.idx
         ${CMAKE_CURRENT_SOURCE_DIR}/data/basic.csv)
set_tests_properties(cli_index PROPERTIES LABELS "cli")

# Differential replay is opt-in: it needs the Rust reference binary.
set(XSV_RUST_XSV_BIN "" CACHE FILEPATH
    "Path to the Rust xsv 0.13.0 binary for differential tests")
if(XSV_RUST_XSV_BIN)
  add_test(NAME differential
           COMMAND ${CMAKE_COMMAND} -E env
             XSV_CPP_BIN=$<TARGET_FILE:xsv>
             XSV_RUST_BIN=${XSV_RUST_XSV_BIN}
             python3 ${CMAKE_CURRENT_SOURCE_DIR}/../scripts/run_correctness.py)
  set_tests_properties(differential PROPERTIES LABELS "differential")
endif()
