add_library(
  aegis_execution STATIC
  src/decoders.cpp
  src/journal.cpp
  src/live_capability.cpp
  src/router.cpp
  src/router_types.cpp
  src/session.cpp
  src/simulated_gateway.cpp
  src/types.cpp
)
add_library(aegis::execution ALIAS aegis_execution)

target_include_directories(
  aegis_execution
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_execution PUBLIC cxx_std_20)
target_compile_definitions(
  aegis_execution
  PUBLIC AEGIS_LIVE_TRADING_COMPILED=$<BOOL:${AEGIS_ENABLE_LIVE_TRADING}>
)
target_link_libraries(
  aegis_execution
  PUBLIC
    aegis::common
    aegis::oms
    aegis::risk
    aegis::synthetic_exchange
)
aegis_apply_project_options(aegis_execution)

install(
  TARGETS aegis_execution
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_execution_tests
    tests/certification_test.cpp
    tests/decoder_recovery_test.cpp
    tests/journal_test.cpp
    tests/live_capability_test.cpp
    tests/paper_gateway_test.cpp
    tests/router_test.cpp
    tests/safety_gate_test.cpp
    tests/types_session_test.cpp
  )
  target_link_libraries(
    aegis_execution_tests
    PRIVATE aegis::execution GTest::gtest_main
  )
  aegis_apply_project_options(aegis_execution_tests)
  add_test(NAME aegis_execution_tests COMMAND aegis_execution_tests)
endif()

if(AEGIS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AEGIS_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(
    aegis_execution PRIVATE -fsanitize=fuzzer-no-link,undefined
  )
  add_executable(
    aegis_gateway_decoder_fuzz
    fuzz/gateway_decoder_fuzz.cpp
  )
  target_link_libraries(aegis_gateway_decoder_fuzz PRIVATE aegis::execution)
  target_compile_options(
    aegis_gateway_decoder_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  target_link_options(
    aegis_gateway_decoder_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  aegis_apply_project_options(aegis_gateway_decoder_fuzz)
endif()
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)
