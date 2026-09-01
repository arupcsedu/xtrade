add_library(
  aegis_models STATIC
  src/baselines.cpp
  src/calibration.cpp
  src/microstructure.cpp
  src/microstructure_dataset.cpp
  src/runner.cpp
  src/types.cpp
  src/validator.cpp
)
add_library(aegis::models ALIAS aegis_models)

target_include_directories(
  aegis_models
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_models PUBLIC cxx_std_20)
target_link_libraries(
  aegis_models PUBLIC aegis::features aegis::event_bus aegis::synthetic_exchange
)
aegis_apply_project_options(aegis_models)

install(
  TARGETS aegis_models
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
)

add_executable(
  synth_microstructure_dataset tools/synthetic_dataset.cpp
)
set_target_properties(
  synth_microstructure_dataset PROPERTIES OUTPUT_NAME "synth-microstructure-dataset"
)
target_link_libraries(synth_microstructure_dataset PRIVATE aegis::models)
aegis_apply_project_options(synth_microstructure_dataset)
install(
  TARGETS synth_microstructure_dataset RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)

add_executable(native_model_verify tools/native_model_verify.cpp)
set_target_properties(native_model_verify PROPERTIES OUTPUT_NAME "native-model-verify")
target_link_libraries(native_model_verify PRIVATE aegis::models)
aegis_apply_project_options(native_model_verify)
install(TARGETS native_model_verify RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(
    aegis_model_tests
    tests/async_runner_test.cpp
    tests/baselines_test.cpp
    tests/calibration_test.cpp
    tests/model_contract_test.cpp
    tests/microstructure_test.cpp
    tests/runner_cache_test.cpp
    tests/validator_test.cpp
  )
  target_link_libraries(aegis_model_tests PRIVATE aegis::models GTest::gtest_main)
  target_compile_definitions(
    aegis_model_tests PRIVATE AEGIS_SOURCE_DIR="${PROJECT_SOURCE_DIR}"
  )
  aegis_apply_project_options(aegis_model_tests)
  add_test(NAME aegis_model_tests COMMAND aegis_model_tests)
endif()
