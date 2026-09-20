# Strict warning set applied to every project target through tradebot_options.
function(tradebot_set_warnings target warnings_as_errors)
  set(gcc_clang_warnings
    -Wall
    -Wextra
    -Wpedantic
    -Wshadow
    -Wconversion
    -Wsign-conversion
    -Wnon-virtual-dtor
    -Wold-style-cast
    -Wcast-align
    -Wunused
    -Woverloaded-virtual
    -Wnull-dereference
    -Wdouble-promotion
    -Wformat=2
    -Wimplicit-fallthrough)

  set(gcc_only_warnings
    -Wmisleading-indentation
    -Wduplicated-cond
    -Wduplicated-branches
    -Wlogical-op
    -Wuseless-cast)

  if(CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    set(warnings ${gcc_clang_warnings})
  elseif(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    set(warnings ${gcc_clang_warnings} ${gcc_only_warnings})
  else()
    message(WARNING "No warning set configured for compiler ${CMAKE_CXX_COMPILER_ID}")
    return()
  endif()

  if(warnings_as_errors)
    list(APPEND warnings -Werror)
  endif()

  target_compile_options(${target} INTERFACE ${warnings})
endfunction()
