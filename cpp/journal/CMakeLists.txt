find_package(Threads REQUIRED)

add_library(
  aegis_journal STATIC
  src/async_journal.cpp
  src/format.cpp
  src/journal.cpp
)
add_library(aegis::journal ALIAS aegis_journal)

target_include_directories(
  aegis_journal
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_journal PUBLIC cxx_std_20)
target_link_libraries(aegis_journal PUBLIC aegis::common Threads::Threads)
aegis_apply_project_options(aegis_journal)

add_library(aegis_journal_cli STATIC src/cli.cpp)
target_link_libraries(aegis_journal_cli PUBLIC aegis::journal)
aegis_apply_project_options(aegis_journal_cli)

foreach(tool_name inspect verify repair_copy export replay)
  add_executable(journal_${tool_name} tools/journal_${tool_name}.cpp)
  target_link_libraries(journal_${tool_name} PRIVATE aegis_journal_cli)
  aegis_apply_project_options(journal_${tool_name})
  string(REPLACE "_" "-" tool_output_name "journal-${tool_name}")
  set_target_properties(journal_${tool_name} PROPERTIES OUTPUT_NAME ${tool_output_name})
endforeach()

install(
  TARGETS aegis_journal
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  TARGETS journal_inspect journal_verify journal_repair_copy journal_export
          journal_replay
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_journal_tests
    tests/async_journal_test.cpp
    tests/journal_test.cpp
    tests/recovery_test.cpp
    tests/tooling_test.cpp
  )
  target_link_libraries(
    aegis_journal_tests PRIVATE aegis::journal aegis_journal_cli GTest::gtest_main
  )
  target_compile_definitions(
    aegis_journal_tests PRIVATE AEGIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  aegis_apply_project_options(aegis_journal_tests)
  add_test(NAME aegis_journal_tests COMMAND aegis_journal_tests)
endif()

if(AEGIS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AEGIS_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(aegis_journal PRIVATE -fsanitize=fuzzer-no-link,undefined)
  add_executable(aegis_journal_frame_fuzz fuzz/journal_frame_fuzz.cpp)
  target_link_libraries(aegis_journal_frame_fuzz PRIVATE aegis::journal)
  target_compile_options(
    aegis_journal_frame_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  target_link_options(
    aegis_journal_frame_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  aegis_apply_project_options(aegis_journal_frame_fuzz)
endif()
