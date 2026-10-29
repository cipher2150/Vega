# Applied globally (including GoogleTest) so instrumented and uninstrumented code never mix:
# TSan in particular reports false races if part of the program is not instrumented.
if(CB_SANITIZER STREQUAL "asan-ubsan")
  set(_cb_san_flags -fsanitize=address,undefined -fno-sanitize-recover=undefined -fno-omit-frame-pointer)
elseif(CB_SANITIZER STREQUAL "tsan")
  set(_cb_san_flags -fsanitize=thread -fno-omit-frame-pointer)
elseif(NOT CB_SANITIZER STREQUAL "")
  message(FATAL_ERROR "Unknown CB_SANITIZER='${CB_SANITIZER}' (use asan-ubsan or tsan)")
endif()

if(_cb_san_flags)
  add_compile_options(${_cb_san_flags})
  add_link_options(${_cb_san_flags})
  message(STATUS "Sanitizers: ${CB_SANITIZER}")
endif()
