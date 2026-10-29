# Strict warnings on our own targets only; third-party code is compiled with its defaults.
function(cb_set_warnings target)
  target_compile_options(${target} PRIVATE
    -Wall -Wextra -Wpedantic -Wshadow -Wconversion -Wsign-conversion
    -Wnon-virtual-dtor -Wold-style-cast -Wcast-align -Wnull-dereference
    -Wimplicit-fallthrough)
  if(CB_WARNINGS_AS_ERRORS)
    target_compile_options(${target} PRIVATE -Werror)
  endif()
endfunction()
