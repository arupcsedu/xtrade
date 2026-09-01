add_library(
  aegis_oms STATIC
  src/journal.cpp
  src/serialization.cpp
  src/service.cpp
  src/types.cpp
)
add_library(aegis::oms ALIAS aegis_oms)

target_include_directories(
  aegis_oms
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_oms PUBLIC cxx_std_20)
target_link_libraries(aegis_oms PUBLIC aegis::common aegis::event_bus aegis::risk)
aegis_apply_project_options(aegis_oms)

install(
  TARGETS aegis_oms
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_oms_tests
    tests/journal_test.cpp
    tests/property_test.cpp
    tests/recovery_test.cpp
    tests/serialization_test.cpp
    tests/service_test.cpp
    tests/types_test.cpp
  )
  target_link_libraries(aegis_oms_tests PRIVATE aegis::oms GTest::gtest_main)
  aegis_apply_project_options(aegis_oms_tests)
  add_test(NAME aegis_oms_tests COMMAND aegis_oms_tests)
endif()

if(AEGIS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AEGIS_BUILD_FUZZERS requires Clang")
  endif()
  add_executable(aegis_oms_state_fuzz fuzz/oms_state_fuzz.cpp)
  target_link_libraries(aegis_oms_state_fuzz PRIVATE aegis::oms)
  target_compile_options(aegis_oms_state_fuzz PRIVATE -fsanitize=fuzzer,undefined)
  target_link_options(aegis_oms_state_fuzz PRIVATE -fsanitize=fuzzer,undefined)
  aegis_apply_project_options(aegis_oms_state_fuzz)
endif()
