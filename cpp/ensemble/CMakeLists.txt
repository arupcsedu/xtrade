add_library(
  aegis_ensemble STATIC src/gate.cpp src/serialization.cpp src/types.cpp
)
add_library(aegis::ensemble ALIAS aegis_ensemble)

target_include_directories(
  aegis_ensemble
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_ensemble PUBLIC cxx_std_20)
target_link_libraries(aegis_ensemble PUBLIC aegis::models aegis::common)
aegis_apply_project_options(aegis_ensemble)

install(
  TARGETS aegis_ensemble
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
    aegis_ensemble_tests
    tests/gate_test.cpp
    tests/property_test.cpp
    tests/serialization_test.cpp
    tests/types_test.cpp
  )
  target_link_libraries(
    aegis_ensemble_tests PRIVATE aegis::ensemble GTest::gtest_main
  )
  aegis_apply_project_options(aegis_ensemble_tests)
  add_test(NAME aegis_ensemble_tests COMMAND aegis_ensemble_tests)
endif()
