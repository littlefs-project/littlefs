cmake_minimum_required(VERSION 3.15)

add_library(${UNIT} STATIC)

target_sources(${UNIT} PRIVATE ${${UNIT}_SOURCES})

target_include_directories(${UNIT} PRIVATE ${${UNIT}_INCLUDES})
target_include_directories(${UNIT} INTERFACE ${${UNIT}_INTERFACE})
target_link_libraries(${UNIT} PRIVATE ${${PROJECT_NAME}_DEPENDENCIES})
target_compile_definitions(${UNIT} PRIVATE ${${UNIT}_DEFINES})