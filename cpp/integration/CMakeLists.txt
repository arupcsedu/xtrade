add_library(
  aegis_paper_integration STATIC
  src/alpaca_paper_path.cpp
  src/paper_acceptance.cpp
)
add_library(aegis::paper_integration ALIAS aegis_paper_integration)

target_include_directories(
  aegis_paper_integration
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_paper_integration PUBLIC cxx_std_20)
target_link_libraries(
  aegis_paper_integration
  PUBLIC
    aegis::common
    aegis::ensemble
    aegis::event_bus
    aegis::execution
    aegis::feed_handler
    aegis::features
    aegis::high_availability
    aegis::models
    aegis::observability
    aegis::oms
    aegis::order_book
    aegis::risk
    aegis::synthetic_exchange
)
aegis_apply_project_options(aegis_paper_integration)

add_library(
  aegis_operator_simulation_core STATIC
  src/operator_simulation.cpp
)
add_library(aegis::operator_simulation_core ALIAS aegis_operator_simulation_core)

target_include_directories(
  aegis_operator_simulation_core
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_operator_simulation_core PUBLIC cxx_std_20)
target_link_libraries(
  aegis_operator_simulation_core
  PUBLIC aegis::common aegis::risk
)
aegis_apply_project_options(aegis_operator_simulation_core)

add_executable(aegis_paper_acceptance tools/paper_acceptance_main.cpp)
target_link_libraries(aegis_paper_acceptance PRIVATE aegis::paper_integration)
aegis_apply_project_options(aegis_paper_acceptance)
set_target_properties(
  aegis_paper_acceptance PROPERTIES OUTPUT_NAME aegis-paper-acceptance
)

add_executable(aegis_alpaca_paper_path tools/alpaca_paper_path_main.cpp)
target_link_libraries(aegis_alpaca_paper_path PRIVATE aegis::paper_integration)
aegis_apply_project_options(aegis_alpaca_paper_path)
set_target_properties(
  aegis_alpaca_paper_path PROPERTIES OUTPUT_NAME aegis-alpaca-paper-path
)

add_executable(aegis_paper_soak_load tools/paper_soak_load.cpp)
target_link_libraries(
  aegis_paper_soak_load
  PRIVATE aegis::common aegis::synthetic_exchange
)
aegis_apply_project_options(aegis_paper_soak_load)
set_target_properties(
  aegis_paper_soak_load PROPERTIES OUTPUT_NAME aegis-paper-soak-load
)

add_executable(aegis_operator_simulation tools/operator_simulation_main.cpp)
target_link_libraries(
  aegis_operator_simulation
  PRIVATE aegis::operator_simulation_core
)
aegis_apply_project_options(aegis_operator_simulation)
set_target_properties(
  aegis_operator_simulation PROPERTIES OUTPUT_NAME aegis-operator-simulation
)

install(
  TARGETS aegis_operator_simulation_core aegis_paper_integration
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  TARGETS
    aegis_alpaca_paper_path
    aegis_operator_simulation
    aegis_paper_acceptance
    aegis_paper_soak_load
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_operator_simulation_tests
    tests/alpaca_paper_path_test.cpp
    tests/operator_simulation_test.cpp
  )
  target_link_libraries(
    aegis_operator_simulation_tests
    PRIVATE
      aegis::operator_simulation_core
      aegis::paper_integration
      GTest::gtest_main
  )
  aegis_apply_project_options(aegis_operator_simulation_tests)
  add_test(
    NAME aegis_operator_simulation_tests
    COMMAND aegis_operator_simulation_tests
  )

  add_test(
    NAME aegis_paper_integration_tests
    COMMAND
      aegis_paper_acceptance
      --machine "${CMAKE_CURRENT_BINARY_DIR}/acceptance-report.json"
      --human "${CMAKE_CURRENT_BINARY_DIR}/system-report.md"
  )
  add_test(
    NAME aegis_paper_soak_load_smoke
    COMMAND
      aegis_paper_soak_load
      --events 8192
      --session-events 2048
      --sample-limit 256
      --realtime-seconds 0
      --machine "${CMAKE_CURRENT_BINARY_DIR}/soak-load-report.json"
      --raw "${CMAKE_CURRENT_BINARY_DIR}/soak-load-samples.ndjson"
  )
  add_test(
    NAME aegis_operator_simulation_evidence
    COMMAND
      aegis_operator_simulation
      --machine "${CMAKE_CURRENT_BINARY_DIR}/operator-simulation.json"
      --audit "${CMAKE_CURRENT_BINARY_DIR}/operator-audit.ndjson"
      --seed 20260908
  )
endif()
