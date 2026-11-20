cmake_minimum_required(VERSION 3.28)
project(cpp_anything VERSION 0.5.0 LANGUAGES CXX)

# The project is built as C++20 modules and imports the standard library
# (`import std;`). GCC 15 is the first release that ships the `std` module
# source (bits/std.cc); MSVC 19.44+ and Clang 18+ with libc++ modules work too.
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 15)
  message(FATAL_ERROR
    "cpp-anything uses C++20 modules and `import std;`: GCC 15+ is required (found ${CMAKE_CXX_COMPILER_VERSION}).")
endif()
if(CMAKE_CXX_COMPILER_ID MATCHES "Clang" AND CMAKE_CXX_COMPILER_VERSION VERSION_LESS 18)
  message(FATAL_ERROR
    "cpp-anything uses C++20 modules and `import std;`: Clang 18+ with libc++ modules is required.")
endif()

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
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/probe.py" probe_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/sitecustomize.py.in" sitecustomize_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/wire.hpp" wire_hpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/driver_main.cpp" driver_main_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/shadow.cpp" shadow_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/shadow_module.py.in" shadow_module_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/batch.py.in" batch_py)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/resources/harness_c.cpp.in" harness_c_cpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/include/cppan/json.hpp" json_hpp)
cppan_embed_file("${CMAKE_CURRENT_SOURCE_DIR}/skills/cpp-anything/SKILL.md" skill_md)

# The standard library module: GCC ships its source (bits/std.cc), and CMake
# needs it as a module source so every import resolves to one BMI.
find_file(CPPAN_STD_MODULE_SOURCE bits/std.cc
  PATHS ${CMAKE_CXX_IMPLICIT_INCLUDE_DIRECTORIES}
  NO_DEFAULT_PATH)
if(NOT CPPAN_STD_MODULE_SOURCE)
  message(FATAL_ERROR
    "bits/std.cc (the libstdc++ `std` module source) was not found; cpp-anything needs GCC 15+ and `import std;`.")
endif()
# CMake file sets must live under the target's source tree, so the one generated
# copy (the compiler's own std module source) goes into an ignored directory.
set(CPPAN_STD_MODULE_DIR "${CMAKE_CURRENT_SOURCE_DIR}/.generated/modules")
file(MAKE_DIRECTORY "${CPPAN_STD_MODULE_DIR}")
file(COPY_FILE "${CPPAN_STD_MODULE_SOURCE}" "${CPPAN_STD_MODULE_DIR}/std.cc")
set_source_files_properties("${CPPAN_STD_MODULE_DIR}/std.cc"
  PROPERTIES COMPILE_OPTIONS "-w")

file(GLOB CPPAN_MODULE_SOURCES CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/src/modules/*.cppm")

add_library(cppan_core STATIC)
target_sources(cppan_core
  PUBLIC
    FILE_SET CXX_MODULES
    BASE_DIRS "${CMAKE_CURRENT_SOURCE_DIR}"
    FILES "${CPPAN_STD_MODULE_DIR}/std.cc" ${CPPAN_MODULE_SOURCES}
  PRIVATE
    src/args.cpp
    src/util.cpp
    src/matrix.cpp
    src/docs.cpp
    src/libs.cpp
    src/types.cpp
    src/inventory.cpp
    src/corpora.cpp
    src/project.cpp
    src/python.cpp
    src/langc.cpp
    src/modules.cpp
    src/codegen.cpp
    src/build.cpp
    src/diff.cpp
    src/bench.cpp
    src/gate.cpp
    src/shadow.cpp
    src/setup.cpp
    src/doctor.cpp
    src/mcp.cpp
    src/app.cpp
    src/appcase.cpp
    src/appfront.cpp
    src/apphttp.cpp
    src/portmap.cpp
    src/profile.cpp
    src/rsi.cpp
    src/recording.cpp
    src/modern.cpp
    src/langs.cpp
    src/langs/go.cpp
    src/langs/rust.cpp
    src/langs/java.cpp
    src/langs/php.cpp
    src/langs/csharp.cpp
    src/langs/ruby.cpp
    src/langs/ts.cpp
    src/analyze.cpp
    src/cli.cpp
)
target_include_directories(cppan_core PRIVATE
  "${CMAKE_CURRENT_SOURCE_DIR}/include"
  "${CMAKE_BINARY_DIR}/generated"
)
target_compile_features(cppan_core PUBLIC cxx_std_23)
target_compile_definitions(cppan_core PUBLIC CPPAN_VERSION="${PROJECT_VERSION}")

if(MSVC)
  target_compile_options(cppan_core PRIVATE /W4 /permissive- /Zc:__cplusplus /utf-8)
else()
  target_compile_options(cppan_core PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow -Wnon-virtual-dtor)
  if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    # GCC 15's optimizer ICEs (nonnull_arg_p) on several module units at -O3 when
    # builtins are folded; disabling builtin folding keeps the build reliable.
    target_compile_options(cppan_core PRIVATE -fno-builtin)
  endif()
endif()

if(CPPAN_SANITIZE)
  add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
  add_link_options(-fsanitize=address,undefined)
endif()

add_executable(cpp-anything src/main.cpp)
target_link_libraries(cpp-anything PRIVATE cppan_core)

include(GNUInstallDirs)
install(TARGETS cpp-anything RUNTIME DESTINATION ${CMAKE_INSTALL_BINDIR})

enable_testing()
add_subdirectory(tests)
