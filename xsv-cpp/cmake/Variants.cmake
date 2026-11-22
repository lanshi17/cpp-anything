# An ablation variant is one (build dir, five compile-time switches) tuple.
# CMakePresets.json wires the named variants used by docs/BENCHMARKS.md; this
# file only records the effective configuration in the binary and in CTest.

if(XSV_WITH_PMR)
  set(_xsv_axis_pmr "P")
else()
  set(_xsv_axis_pmr "-")
endif()
if(XSV_WITH_SIMD)
  set(_xsv_axis_simd "S")
else()
  set(_xsv_axis_simd "-")
endif()
if(XSV_WITH_TBB)
  set(_xsv_axis_tbb "T")
else()
  set(_xsv_axis_tbb "-")
endif()
if(XSV_WITH_CUSTOM_HASH)
  set(_xsv_axis_hash "H")
else()
  set(_xsv_axis_hash "-")
endif()
if(XSV_WITH_MMAP)
  set(_xsv_axis_mmap "M")
else()
  set(_xsv_axis_mmap "-")
endif()

set(XSV_AXES "${_xsv_axis_pmr}${_xsv_axis_simd}${_xsv_axis_tbb}${_xsv_axis_hash}${_xsv_axis_mmap}")
message(STATUS "xsv-cpp variant '${XSV_VARIANT_NAME}' axes [${XSV_AXES}] "
               "(P=pmr S=simd T=tbb H=hash M=mmap)")
