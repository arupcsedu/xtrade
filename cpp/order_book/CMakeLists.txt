add_library(
  aegis_order_book STATIC
  src/book_universe.cpp
  src/order_book.cpp
  src/synthetic_adapter.cpp
)
add_library(aegis::order_book ALIAS aegis_order_book)

target_include_directories(
  aegis_order_book
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_order_book PUBLIC cxx_std_20)
target_link_libraries(aegis_order_book PUBLIC aegis::synthetic_exchange)
aegis_apply_project_options(aegis_order_book)

install(
  TARGETS aegis_order_book
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
    aegis_order_book_tests
    tests/order_book_test.cpp
    tests/order_book_property_test.cpp
  )
  target_link_libraries(
    aegis_order_book_tests PRIVATE aegis::order_book GTest::gtest_main
  )
  aegis_apply_project_options(aegis_order_book_tests)
  add_test(NAME aegis_order_book_tests COMMAND aegis_order_book_tests)
endif()
