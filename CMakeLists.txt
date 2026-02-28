# littlefs 

add_library(littlefs INTERFACE)

target_include_directories(littlefs INTERFACE
  ${CMAKE_CURRENT_LIST_DIR}
)

target_sources(littlefs INTERFACE
  lfs.c
  lfs_util.c
)

if (CMAKE_C_COMPILER_ID MATCHES "GNU|Clang")
  target_compile_options(littlefs INTERFACE "-Wno-unused-function")
endif()
