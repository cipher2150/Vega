set(CB_TEST_DATA_DIR ${CMAKE_CURRENT_SOURCE_DIR}/data)

add_executable(cb_tests
  unit/test_decimal.cpp
  unit/test_spsc_ring.cpp
  unit/test_book.cpp
  unit/test_platform.cpp
  unit/test_kraken.cpp
  unit/test_protocols.cpp
  unit/test_engine.cpp
  unit/test_golden.cpp)
target_include_directories(cb_tests PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cb_tests PRIVATE CB_TEST_DATA_DIR="${CB_TEST_DATA_DIR}")
target_link_libraries(cb_tests PRIVATE cb_lib GTest::gtest_main)
cb_set_warnings(cb_tests)

# Separate binary: it replaces global operator new to count allocations, which must not
# leak into the other tests.
add_executable(cb_alloc_test alloc/test_alloc.cpp)
target_include_directories(cb_alloc_test PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(cb_alloc_test PRIVATE CB_TEST_DATA_DIR="${CB_TEST_DATA_DIR}")
target_link_libraries(cb_alloc_test PRIVATE cb_lib GTest::gtest)
cb_set_warnings(cb_alloc_test)

include(GoogleTest)
gtest_discover_tests(cb_tests DISCOVERY_TIMEOUT 60)
# ASan/TSan replace operator new themselves; counting is meaningless there.
if(CB_SANITIZER STREQUAL "")
  gtest_discover_tests(cb_alloc_test DISCOVERY_TIMEOUT 60)
endif()
