add_library(
  aegis_synthetic_exchange STATIC
  src/artifacts.cpp
  src/book.cpp
  src/canonical.cpp
  src/capture.cpp
  src/config.cpp
  src/event.cpp
  src/generator.cpp
  src/mock_protocol.cpp
)
add_library(aegis::synthetic_exchange ALIAS aegis_synthetic_exchange)

target_include_directories(
  aegis_synthetic_exchange
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_synthetic_exchange PUBLIC cxx_std_20)
target_link_libraries(aegis_synthetic_exchange PUBLIC aegis::common)
aegis_apply_project_options(aegis_synthetic_exchange)

add_library(
  aegis_feed_handler STATIC
  src/feed_arbitrator.cpp
  src/feed_handler.cpp
  src/feed_recovery.cpp
  src/feed_sequence.cpp
  src/feed_synthetic_receiver.cpp
)
add_library(aegis::feed_handler ALIAS aegis_feed_handler)
target_include_directories(
  aegis_feed_handler
  PUBLIC
    $<BUILD_INTERFACE:${CMAKE_CURRENT_SOURCE_DIR}/include>
    $<INSTALL_INTERFACE:${CMAKE_INSTALL_INCLUDEDIR}>
)
target_compile_features(aegis_feed_handler PUBLIC cxx_std_20)
target_link_libraries(aegis_feed_handler PUBLIC aegis::synthetic_exchange)
aegis_apply_project_options(aegis_feed_handler)

foreach(tool IN ITEMS generate stream verify inspect)
  string(REPLACE "_" "-" output_suffix "${tool}")
  add_executable(synth_exchange_${tool} tools/${tool}.cpp)
  set_target_properties(
    synth_exchange_${tool}
    PROPERTIES OUTPUT_NAME "synth-exchange-${output_suffix}"
  )
  target_link_libraries(
    synth_exchange_${tool} PRIVATE aegis::synthetic_exchange
  )
  aegis_apply_project_options(synth_exchange_${tool})
  install(TARGETS synth_exchange_${tool} RUNTIME DESTINATION "${CMAKE_INSTALL_BINDIR}")
endforeach()

install(
  TARGETS aegis_synthetic_exchange aegis_feed_handler
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
    aegis_synthetic_exchange_tests
    tests/artifacts_test.cpp
    tests/feed_handler_test.cpp
    tests/generator_test.cpp
    tests/mock_protocol_test.cpp
    tests/property_test.cpp
  )
  target_link_libraries(
    aegis_synthetic_exchange_tests
    PRIVATE aegis::feed_handler GTest::gtest_main
  )
  aegis_apply_project_options(aegis_synthetic_exchange_tests)
  add_test(NAME aegis_synthetic_exchange_tests COMMAND aegis_synthetic_exchange_tests)
endif()

if(AEGIS_BUILD_FUZZERS)
  target_compile_options(
    aegis_synthetic_exchange PRIVATE -fsanitize=fuzzer-no-link,undefined
  )
  target_compile_options(
    aegis_feed_handler PRIVATE -fsanitize=fuzzer-no-link,undefined
  )
  add_executable(aegis_smx_packet_fuzz fuzz/mock_protocol_fuzz.cpp)
  target_link_libraries(
    aegis_smx_packet_fuzz PRIVATE aegis::synthetic_exchange
  )
  target_compile_options(
    aegis_smx_packet_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  target_link_options(
    aegis_smx_packet_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  aegis_apply_project_options(aegis_smx_packet_fuzz)

  add_executable(aegis_feed_decoder_fuzz fuzz/feed_decoder_fuzz.cpp)
  target_link_libraries(
    aegis_feed_decoder_fuzz PRIVATE aegis::feed_handler
  )
  target_compile_options(
    aegis_feed_decoder_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  target_link_options(
    aegis_feed_decoder_fuzz PRIVATE -fsanitize=fuzzer,undefined
  )
  aegis_apply_project_options(aegis_feed_decoder_fuzz)
endif()
