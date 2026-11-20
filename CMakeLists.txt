cmake_minimum_required(VERSION 3.28)
project(cpp_anything VERSION 0.5.0 LANGUAGES CXX)

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

add_library(cppan_core STATIC
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
  src/langs/java.cpp
  src/langs/php.cpp
  src/langs/csharp.cpp
  src/langs/ruby.cpp
  src/langs/ts.cpp
  src/analyze.cpp
  src/cli.cpp
)
target_include_directories(cppan_core
  PUBLIC
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
