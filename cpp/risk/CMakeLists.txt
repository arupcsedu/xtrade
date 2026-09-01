add_library(
  aegis_risk STATIC
  src/engine.cpp
  src/journal.cpp
  src/portfolio_journal.cpp
  src/portfolio_service.cpp
  src/portfolio_types.cpp
  src/serialization.cpp
  src/types.cpp
)
add_library(aegis::risk ALIAS aegis_risk)

target_include_directories(
  aegis_risk
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_risk PUBLIC cxx_std_20)
target_link_libraries(aegis_risk PUBLIC aegis::common aegis::event_bus)
aegis_apply_project_options(aegis_risk)

install(
  TARGETS aegis_risk
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
    aegis_risk_tests
    tests/concurrency_test.cpp
    tests/engine_test.cpp
    tests/journal_test.cpp
    tests/limits_test.cpp
    tests/portfolio_pretrade_test.cpp
    tests/portfolio_replay_test.cpp
    tests/portfolio_service_test.cpp
    tests/property_test.cpp
    tests/serialization_test.cpp
  )
  target_link_libraries(aegis_risk_tests PRIVATE aegis::risk GTest::gtest_main)
  aegis_apply_project_options(aegis_risk_tests)
  add_test(NAME aegis_risk_tests COMMAND aegis_risk_tests)
endif()
