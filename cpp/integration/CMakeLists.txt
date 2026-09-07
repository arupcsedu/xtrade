add_library(
  aegis_paper_integration STATIC
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

add_executable(aegis_paper_acceptance tools/paper_acceptance_main.cpp)
target_link_libraries(aegis_paper_acceptance PRIVATE aegis::paper_integration)
aegis_apply_project_options(aegis_paper_acceptance)
set_target_properties(
  aegis_paper_acceptance PROPERTIES OUTPUT_NAME aegis-paper-acceptance
)

install(
  TARGETS aegis_paper_integration
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)
install(
  TARGETS aegis_paper_acceptance
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_test(
    NAME aegis_paper_integration_tests
    COMMAND
      aegis_paper_acceptance
      --machine "${CMAKE_CURRENT_BINARY_DIR}/acceptance-report.json"
      --human "${CMAKE_CURRENT_BINARY_DIR}/system-report.md"
  )
endif()
