include_guard(GLOBAL)

function(aegis_validate_options)
  if(AEGIS_ENABLE_ASAN AND AEGIS_ENABLE_TSAN)
    message(FATAL_ERROR "AddressSanitizer and ThreadSanitizer require separate builds")
  endif()

  if((AEGIS_ENABLE_ASAN OR AEGIS_ENABLE_UBSAN OR AEGIS_ENABLE_TSAN)
     AND MSVC)
    message(FATAL_ERROR "The configured sanitizer presets require GCC or Clang")
  endif()
endfunction()

function(aegis_configure_ccache)
  if(NOT AEGIS_ENABLE_CCACHE)
    return()
  endif()

  find_program(AEGIS_CCACHE_PROGRAM ccache)
  if(AEGIS_CCACHE_PROGRAM)
    set(CMAKE_CXX_COMPILER_LAUNCHER "${AEGIS_CCACHE_PROGRAM}" PARENT_SCOPE)
  endif()
endfunction()

function(aegis_create_project_option_targets)
  add_library(aegis_project_warnings INTERFACE)
  add_library(aegis::project_warnings ALIAS aegis_project_warnings)

  if(MSVC)
    target_compile_options(aegis_project_warnings INTERFACE /W4 /permissive-)
    if(AEGIS_WARNINGS_AS_ERRORS)
      target_compile_options(aegis_project_warnings INTERFACE /WX)
    endif()
  else()
    target_compile_options(
      aegis_project_warnings
      INTERFACE
        -Wall
        -Wextra
        -Wpedantic
        -Wconversion
        -Wsign-conversion
        -Wshadow
        -Wformat=2
        -Wundef
        -Wnull-dereference
        -Werror=return-type
        $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-cond>
        $<$<CXX_COMPILER_ID:GNU>:-Wduplicated-branches>
        $<$<CXX_COMPILER_ID:GNU>:-Wlogical-op>
    )
    if(AEGIS_WARNINGS_AS_ERRORS)
      target_compile_options(aegis_project_warnings INTERFACE -Werror)
    endif()
  endif()

  add_library(aegis_project_sanitizers INTERFACE)
  add_library(aegis::project_sanitizers ALIAS aegis_project_sanitizers)

  if(AEGIS_ENABLE_ASAN)
    target_compile_options(
      aegis_project_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer
    )
    target_link_options(
      aegis_project_sanitizers INTERFACE -fsanitize=address -fno-omit-frame-pointer
    )
  endif()

  if(AEGIS_ENABLE_UBSAN)
    target_compile_options(
      aegis_project_sanitizers
      INTERFACE -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    )
    target_link_options(
      aegis_project_sanitizers
      INTERFACE -fsanitize=undefined -fno-sanitize-recover=all -fno-omit-frame-pointer
    )
  endif()

  if(AEGIS_ENABLE_TSAN)
    target_compile_options(
      aegis_project_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer
    )
    target_link_options(
      aegis_project_sanitizers INTERFACE -fsanitize=thread -fno-omit-frame-pointer
    )
  endif()
endfunction()

function(aegis_resolve_source_revision output_variable)
  set(resolved_revision "${AEGIS_SOURCE_REVISION}")
  if(NOT resolved_revision)
    find_package(Git QUIET)
    if(Git_FOUND)
      execute_process(
        COMMAND "${GIT_EXECUTABLE}" rev-parse --verify HEAD
        WORKING_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}"
        RESULT_VARIABLE git_result
        OUTPUT_VARIABLE git_revision
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
      )
      if(git_result EQUAL 0)
        set(resolved_revision "${git_revision}")
      endif()
    endif()
  endif()

  if(NOT resolved_revision)
    set(resolved_revision "unversioned")
  endif()

  if(NOT resolved_revision STREQUAL "unversioned")
    string(LENGTH "${resolved_revision}" revision_length)
    if(NOT revision_length EQUAL 40 OR NOT resolved_revision MATCHES "^[0-9a-f]+$")
      message(FATAL_ERROR "AEGIS_SOURCE_REVISION must be 'unversioned' or a 40-character lowercase Git SHA")
    endif()
  endif()

  set(${output_variable} "${resolved_revision}" PARENT_SCOPE)
endfunction()

function(aegis_apply_project_options target_name)
  target_link_libraries(
    ${target_name}
    PRIVATE
      $<BUILD_INTERFACE:aegis::project_warnings>
      $<BUILD_INTERFACE:aegis::project_sanitizers>
  )

  if(AEGIS_ENABLE_CLANG_TIDY)
    find_program(AEGIS_CLANG_TIDY_PROGRAM clang-tidy REQUIRED)
    set_property(
      TARGET ${target_name}
      PROPERTY CXX_CLANG_TIDY
               "${AEGIS_CLANG_TIDY_PROGRAM};--config-file=${PROJECT_SOURCE_DIR}/.clang-tidy"
    )
  endif()
endfunction()
