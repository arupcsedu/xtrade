add_library(aegis_replay STATIC src/replay.cpp)
add_library(aegis::replay ALIAS aegis_replay)

target_include_directories(
  aegis_replay
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_replay PUBLIC cxx_std_20)
target_link_libraries(
  aegis_replay PUBLIC aegis::common aegis::journal aegis::synthetic_exchange aegis::time
)
aegis_apply_project_options(aegis_replay)

add_executable(aegis_replay_tool tools/aegis_replay.cpp)
set_target_properties(aegis_replay_tool PROPERTIES OUTPUT_NAME aegis-replay)
target_link_libraries(aegis_replay_tool PRIVATE aegis::replay)
aegis_apply_project_options(aegis_replay_tool)

install(TARGETS aegis_replay EXPORT AegisMXTargets ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}")
install(TARGETS aegis_replay_tool RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(aegis_replay_tests tests/replay_test.cpp)
  target_link_libraries(aegis_replay_tests PRIVATE aegis::replay GTest::gtest_main)
  aegis_apply_project_options(aegis_replay_tests)
  add_test(NAME aegis_replay_tests COMMAND aegis_replay_tests)
endif()

