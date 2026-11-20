cmake_minimum_required(VERSION 3.28)
project(cpp_anything VERSION 0.4.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

option(CPPAN_SANITIZE "Build with AddressSanitizer and UndefinedBehaviorSanitizer" OFF)

include(cmake/Embed.cmake)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/data/matrix.json" matrix_json)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/data/libs.json" libs_json)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/probe.py" probe_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/sitecustomize.py.in" sitecustomize_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/wire.hpp" wire_hpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/main.cpp" driver_main_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/shadow.cpp" shadow_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/shadow_module.py.in" shadow_module_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/batch.py.in" batch_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/src/cpp_runtime/harness_c.cpp.in" harness_c_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/native/include/cppan/json.hpp" json_hpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/skills/cpp-anything/SKILL.md" skill_md)

add_library(cppan_core STATIC
  native/src/util.cpp
  native/src/matrix.cpp
  native/src/docs.cpp
  native/src/libs.cpp
  native/src/types.cpp
  native/src/inventory.cpp
  native/src/corpora.cpp
  native/src/project.cpp
  native/src/python.cpp
  native/src/langc.cpp
  native/src/modules.cpp
  native/src/codegen.cpp
  native/src/build.cpp
  native/src/diff.cpp
  native/src/bench.cpp
  native/src/gate.cpp
  native/src/shadow.cpp
  native/src/setup.cpp
  native/src/doctor.cpp
  native/src/mcp.cpp
  native/src/app.cpp
  native/src/appcase.cpp
  native/src/appfront.cpp
  native/src/apphttp.cpp
  native/src/portmap.cpp
  native/src/profile.cpp
  native/src/rsi.cpp
  native/src/recording.cpp
  native/src/modern.cpp
  native/src/langs.cpp
  native/src/langs/go.cpp
  native/src/langs/java.cpp
  native/src/langs/php.cpp
  native/src/langs/csharp.cpp
  native/src/langs/ruby.cpp
  native/src/langs/ts.cpp
  native/src/analyze.cpp
  native/src/cli.cpp
)
target_include_directories(cppan_core
  PUBLIC
    "${CMAKE_CURRENT_SOURCE_DIR}/native/include"
    "${CMAKE_BINARY_DIR}/generated"
)
target_compile_features(cppan_core PUBLIC cxx_std_23)
target_compile_definitions(cppan_core PUBLIC CPPAN_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(cppan_core PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(cppan_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
endif()

if(CPPAN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_executable(cpp-anything native/src/main.cpp)
target_link_libraries(cpp-anything PRIVATE cppan_core)

enable_testing()
add_subdirectory(native/tests)
