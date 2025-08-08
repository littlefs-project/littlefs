cmake_minimum_required(VERSION 3.23)
project(${UNIT}_unit_test)

add_library(${UNIT}_unit_tests OBJECT)

target_sources(${UNIT}_unit_tests PRIVATE
    ${${UNIT}_C}
    ${${UNIT}_unit_tests_C}
)

target_include_directories  (${UNIT}_unit_tests PUBLIC ${${UNIT}_UNIT_TESTS_INCLUDE})
target_link_libraries       (${UNIT}_unit_tests PUBLIC gtest gmock atomic freertos_mock)
target_compile_definitions  (${UNIT}_unit_tests PUBLIC ${${UNIT}_UNIT_TEST_DEFINES})