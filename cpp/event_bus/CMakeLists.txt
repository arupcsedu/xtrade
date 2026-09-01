add_library(
  aegis_event_bus STATIC
  src/process_epoch.cpp
)
add_library(aegis::event_bus ALIAS aegis_event_bus)

target_include_directories(
  aegis_event_bus
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_event_bus PUBLIC cxx_std_20)
target_link_libraries(aegis_event_bus PUBLIC aegis::common)
aegis_apply_project_options(aegis_event_bus)

install(
  TARGETS aegis_event_bus
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
    aegis_event_bus_tests
    tests/event_bus_test.cpp
    tests/concurrency_test.cpp
    tests/shared_memory_test.cpp
  )
  target_link_libraries(
    aegis_event_bus_tests PRIVATE aegis::event_bus GTest::gtest_main
  )
  aegis_apply_project_options(aegis_event_bus_tests)
  add_test(NAME aegis_event_bus_tests COMMAND aegis_event_bus_tests)
endif()
