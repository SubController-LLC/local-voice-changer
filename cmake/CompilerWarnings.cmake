function(vvc_set_project_warnings target)
  if(MSVC)
    target_compile_options(${target} PRIVATE /W4 /WX /permissive- /utf-8)
  else()
    target_compile_options(
      ${target}
      PRIVATE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Werror)
  endif()
endfunction()

function(vvc_enable_sanitizers target)
  if(NOT VVC_ENABLE_SANITIZERS)
    return()
  endif()

  if(MSVC)
    message(WARNING "VVC_ENABLE_SANITIZERS is not enabled for MSVC in this project")
  else()
    target_compile_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
    target_link_options(${target} PRIVATE -fsanitize=address,undefined -fno-omit-frame-pointer)
  endif()
endfunction()
