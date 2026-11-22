# Sanitizers instrument xsv-cpp only. Third-party dependencies are built
# unsanitized: instrumenting Abseil breaks its constexpr function-pointer
# checks, and a sanitized TBB would report races inside its own scheduler.
#
# ASan+UBSan and TSan are separate build directories; C++ sanitizers cannot be
# mixed in one binary. The benchmark build must never carry sanitizer flags.

# TSan must instrument the dependency code too: an uninstrumented oneTBB uses
# synchronization TSan cannot see, which produces false races inside the
# scheduler. UBSan+ASan is the opposite case, so it stays target-scoped.
if(XSV_SANITIZE MATCHES "thread")
  add_compile_options(-fsanitize=thread -fno-omit-frame-pointer -g)
  add_link_options(-fsanitize=thread -fno-omit-frame-pointer)
endif()

function(xsv_apply_sanitizers target)
  if(NOT XSV_SANITIZE)
    return()
  endif()
  string(REPLACE ";" "," _xsv_sanitize_list "${XSV_SANITIZE}")
  target_compile_options(${target} PRIVATE
    -fsanitize=${_xsv_sanitize_list} -fno-omit-frame-pointer -g)
  target_link_options(${target} PRIVATE
    -fsanitize=${_xsv_sanitize_list} -fno-omit-frame-pointer)
  if(XSV_SANITIZE MATCHES "address")
    target_compile_definitions(${target} PRIVATE XSV_ASAN=1)
  endif()
  if(XSV_SANITIZE MATCHES "thread")
    target_compile_definitions(${target} PRIVATE XSV_TSAN=1)
  endif()
endfunction()
