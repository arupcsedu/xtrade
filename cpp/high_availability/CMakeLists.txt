add_library(
  aegis_high_availability STATIC
  src/coordinator.cpp
  src/recovery_stream.cpp
)
add_library(aegis::high_availability ALIAS aegis_high_availability)

target_include_directories(
  aegis_high_availability
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_high_availability PUBLIC cxx_std_20)
target_link_libraries(
  aegis_high_availability
  PUBLIC aegis::common aegis::event_bus
)
aegis_apply_project_options(aegis_high_availability)

install(
  TARGETS aegis_high_availability
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
    aegis_high_availability_tests
    tests/coordinator_test.cpp
    tests/failover_integration_test.cpp
    tests/recovery_stream_test.cpp
  )
  target_link_libraries(
    aegis_high_availability_tests
    PRIVATE aegis::high_availability GTest::gtest_main
  )
  aegis_apply_project_options(aegis_high_availability_tests)
  add_test(NAME aegis_high_availability_tests COMMAND aegis_high_availability_tests)
endif()
