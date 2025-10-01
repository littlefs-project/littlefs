cmake_minimum_required(VERSION 3.23)
project(${UNIT}_standalone)
set(CMAKE_CXX_STANDARD 17)

include(FetchContent)

FetchContent_Declare(
   gtest
   GIT_REPOSITORY /some_folder/googletest
   GIT_TAG v1.14.0
)

FetchContent_GetProperties(gtest)

if(NOT gtest_POPULATED)
   FetchContent_Populate(gtest)
   add_subdirectory(${gtest_SOURCE_DIR} ${gtest_BINARY_DIR})
endif()

FetchContent_Declare(
   freertos
   GIT_REPOSITORY git@git.evm.eco:evm-software/embedded/system/os/freertos.git
   GIT_TAG ${freertos_tag}
)

FetchContent_GetProperties(freertos)

if(NOT freertos_POPULATED)
   FetchContent_Populate(freertos)
   add_subdirectory(${freertos_SOURCE_DIR} ${freertos_BINARY_DIR} EXCLUDE_FROM_ALL)
endif()


add_executable(${UNIT}_standalone_test
    ${${UNIT}_C}
    ${${UNIT}_unit_tests_C}
)


target_include_directories(${UNIT}_standalone_test PRIVATE ${${UNIT}_UNIT_TESTS_INCLUDE} ${${UNIT}_standalone_includes})

target_compile_definitions(${UNIT}_standalone_test PRIVATE  ${${UNIT}_standalone_defines})

target_link_libraries(${UNIT}_standalone_test PRIVATE freertos_mock gtest gmock gmock_main atomic)
