set(AEGIS_EDGE_SERVICE_TARGETS)

function(aegis_add_edge_service target_name output_name role_name)
  add_executable(${target_name} src/edge_service_main.cpp)
  target_link_libraries(${target_name} PRIVATE aegis::common)
  target_compile_definitions(
    ${target_name} PRIVATE AEGIS_EDGE_SERVICE_ROLE="${role_name}"
  )
  aegis_apply_project_options(${target_name})
  set_target_properties(${target_name} PROPERTIES OUTPUT_NAME "${output_name}")
  list(APPEND AEGIS_EDGE_SERVICE_TARGETS ${target_name})
  set(AEGIS_EDGE_SERVICE_TARGETS "${AEGIS_EDGE_SERVICE_TARGETS}" PARENT_SCOPE)
endfunction()

aegis_add_edge_service(aegis_edge_clock_guard aegis-edge-clock-guard ptp)
aegis_add_edge_service(aegis_edge_journal_service aegis-edge-journal journal)
aegis_add_edge_service(aegis_edge_core_service aegis-edge-core edge-core)
aegis_add_edge_service(aegis_paper_gateway_service aegis-paper-gateway paper-gateway)
aegis_add_edge_service(
  aegis_edge_observability_service aegis-edge-observability observability
)

install(TARGETS ${AEGIS_EDGE_SERVICE_TARGETS} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")

if(AEGIS_BUILD_TESTS)
  add_test(
    NAME aegis_edge_services_reject_missing_configuration
    COMMAND aegis_edge_core_service --configuration /aegis-mx/absent
  )
  set_tests_properties(
    aegis_edge_services_reject_missing_configuration PROPERTIES WILL_FAIL TRUE
  )
endif()
