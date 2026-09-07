add_library(
  aegis_observability STATIC
  src/adapters.cpp
  src/decision_explanation.cpp
  src/structured_log.cpp
  src/telemetry.cpp
  src/tracing.cpp
)
add_library(aegis::observability ALIAS aegis_observability)

target_include_directories(
  aegis_observability
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_observability PUBLIC cxx_std_20)
target_link_libraries(
  aegis_observability
  PUBLIC
    aegis::ensemble
    aegis::event_bus
    aegis::execution
    aegis::journal
    aegis::feed_handler
    aegis::models
    aegis::risk
)
aegis_apply_project_options(aegis_observability)

install(
  TARGETS aegis_observability
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
    aegis_observability_tests
    tests/decision_explanation_test.cpp
    tests/observability_test.cpp
  )
  target_link_libraries(
    aegis_observability_tests PRIVATE aegis::observability GTest::gtest_main
  )
  aegis_apply_project_options(aegis_observability_tests)
  add_test(NAME aegis_observability_tests COMMAND aegis_observability_tests)
endif()
