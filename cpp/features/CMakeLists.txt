add_library(
  aegis_features STATIC
  src/exponential_statistics.cpp
  src/feature_engine.cpp
  src/parity.cpp
  src/publisher.cpp
)
add_library(aegis::features ALIAS aegis_features)

target_include_directories(
  aegis_features
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_features PUBLIC cxx_std_20)
target_link_libraries(aegis_features PUBLIC aegis::order_book)
aegis_apply_project_options(aegis_features)

install(
  TARGETS aegis_features
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
    aegis_feature_tests
    tests/feature_engine_test.cpp
    tests/feature_property_test.cpp
  )
  target_link_libraries(
    aegis_feature_tests PRIVATE aegis::features GTest::gtest_main
  )
  aegis_apply_project_options(aegis_feature_tests)
  add_test(NAME aegis_feature_tests COMMAND aegis_feature_tests)
endif()
