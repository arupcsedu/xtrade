add_library(
  aegis_common STATIC
  src/authentication.cpp
  src/audit_envelope.cpp
  src/build_info.cpp
  src/market_state.cpp
  src/sha256.cpp
)
add_library(aegis::common ALIAS aegis_common)

add_library(
  aegis_time STATIC
  src/clock_jump.cpp
  src/clock_quality.cpp
  src/clocks.cpp
  src/latency.cpp
)
add_library(aegis::time ALIAS aegis_time)
set_target_properties(aegis_time PROPERTIES EXPORT_NAME time)

target_include_directories(
  aegis_common
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<BUILD_INTERFACE:${AEGIS_GENERATED_INCLUDE_DIR}>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_include_directories(
  aegis_common
  SYSTEM PUBLIC
    $<BUILD_INTERFACE:${PROJECT_SOURCE_DIR}/schemas/generated/cpp>
    $<BUILD_INTERFACE:${flatbuffers_SOURCE_DIR}/include>
)
target_compile_features(aegis_common PUBLIC cxx_std_20)
target_link_libraries(aegis_common PUBLIC OpenSSL::Crypto)
aegis_apply_project_options(aegis_common)

target_include_directories(
  aegis_time
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_time PUBLIC cxx_std_20)
aegis_apply_project_options(aegis_time)

install(
  TARGETS aegis_common aegis_time
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)
install(
  FILES "${AEGIS_GENERATED_INCLUDE_DIR}/aegis/common/build_info_generated.hpp"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}/aegis/common"
)
install(
  DIRECTORY "${PROJECT_SOURCE_DIR}/schemas/generated/cpp/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.h"
)
install(
  DIRECTORY "${flatbuffers_SOURCE_DIR}/include/flatbuffers"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
)
install(
  FILES "${flatbuffers_SOURCE_DIR}/LICENSE"
  DESTINATION "${CMAKE_INSTALL_DATADIR}/licenses/aegis-mx/flatbuffers"
)
install(
  EXPORT AegisMXTargets
  NAMESPACE aegis::
  DESTINATION "${CMAKE_INSTALL_LIBDIR}/cmake/AegisMX"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_common_tests
    tests/authentication_test.cpp
    tests/audit_envelope_test.cpp
    tests/build_info_test.cpp
    tests/compatibility_test.cpp
    tests/identifier_test.cpp
    tests/market_state_property_test.cpp
    tests/market_state_test.cpp
    tests/sha256_test.cpp
  )
  target_link_libraries(aegis_common_tests PRIVATE aegis::common GTest::gtest_main)
  target_compile_definitions(
    aegis_common_tests PRIVATE AEGIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  aegis_apply_project_options(aegis_common_tests)
  add_test(NAME aegis_common_tests COMMAND aegis_common_tests)

  add_executable(
    aegis_time_tests
    tests/clock_quality_test.cpp
    tests/clocks_test.cpp
    tests/latency_test.cpp
    tests/time_types_test.cpp
  )
  target_link_libraries(aegis_time_tests PRIVATE aegis::time GTest::gtest_main)
  aegis_apply_project_options(aegis_time_tests)
  add_test(NAME aegis_time_tests COMMAND aegis_time_tests)
endif()

if(AEGIS_BUILD_FUZZERS)
  if(NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "AEGIS_BUILD_FUZZERS requires Clang")
  endif()
  target_compile_options(
    aegis_common PRIVATE -fsanitize=fuzzer-no-link,undefined
  )
  add_executable(
    aegis_audit_envelope_fuzz
    fuzz/audit_envelope_fuzz.cpp
  )
  target_link_libraries(aegis_audit_envelope_fuzz PRIVATE aegis::common)
  target_compile_options(
    aegis_audit_envelope_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  target_link_options(
    aegis_audit_envelope_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  aegis_apply_project_options(aegis_audit_envelope_fuzz)
endif()
