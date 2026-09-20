# AddressSanitizer + UndefinedBehaviorSanitizer for Debug builds.
function(tradebot_enable_sanitizers target)
  if(NOT (CMAKE_CXX_COMPILER_ID MATCHES "Clang" OR CMAKE_CXX_COMPILER_ID STREQUAL "GNU"))
    return()
  endif()
  set(flags -fsanitize=address,undefined -fno-omit-frame-pointer -fno-sanitize-recover=undefined)
  target_compile_options(${target} INTERFACE ${flags})
  target_link_options(${target} INTERFACE ${flags})
endfunction()
