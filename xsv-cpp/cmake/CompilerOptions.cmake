# Flags shared by every translation unit. Kept deliberately close to what the
# Rust reference build sees: one Release profile, LTO on, no debug assertions.

function(xsv_apply_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -Wnon-virtual-dtor -Wold-style-cast -Woverloaded-virtual
    -Wnull-dereference -Wdouble-promotion -Wformat=2)
  if(XSV_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()

function(xsv_apply_optimization target)
  if(CMAKE_BUILD_TYPE STREQUAL "Release" OR CMAKE_BUILD_TYPE STREQUAL "RelWithDebInfo")
    target_compile_options(${target} PRIVATE -O3 -DNDEBUG)
    if(XSV_NATIVE)
      target_compile_options(${target} PRIVATE -march=native -mtune=native)
      target_compile_definitions(${target} PRIVATE XSV_NATIVE_BUILD=1)
    else()
      target_compile_options(${target} PRIVATE -mtune=generic)
      target_compile_definitions(${target} PRIVATE XSV_NATIVE_BUILD=0)
    endif()
    if(XSV_LTO)
      set_property(TARGET ${target} PROPERTY INTERPROCEDURAL_OPTIMIZATION TRUE)
    endif()
  endif()
endfunction()
