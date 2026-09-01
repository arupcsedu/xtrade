add_library(aegis_backtesting STATIC src/backtester.cpp src/report.cpp)
add_library(aegis::backtesting ALIAS aegis_backtesting)

target_include_directories(
  aegis_backtesting
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_backtesting PUBLIC cxx_std_20)
target_link_libraries(
  aegis_backtesting
  PUBLIC aegis::common aegis::synthetic_exchange
)
aegis_apply_project_options(aegis_backtesting)

add_executable(aegis_backtest_tool tools/aegis_backtest.cpp)
set_target_properties(aegis_backtest_tool PROPERTIES OUTPUT_NAME aegis-backtest)
target_link_libraries(aegis_backtest_tool PRIVATE aegis::backtesting)
aegis_apply_project_options(aegis_backtest_tool)

install(
  TARGETS aegis_backtesting aegis_backtest_tool
  EXPORT AegisMXTargets
  ARCHIVE DESTINATION "${CMAKE_INSTALL_LIBDIR}"
  RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}"
)
install(
  DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/include/"
  DESTINATION "${CMAKE_INSTALL_INCLUDEDIR}"
  FILES_MATCHING PATTERN "*.hpp"
)

if(AEGIS_BUILD_TESTS)
  add_executable(aegis_backtesting_tests tests/backtester_test.cpp)
  target_link_libraries(
    aegis_backtesting_tests PRIVATE aegis::backtesting GTest::gtest_main
  )
  aegis_apply_project_options(aegis_backtesting_tests)
  add_test(NAME aegis_backtesting_tests COMMAND aegis_backtesting_tests)
endif()
