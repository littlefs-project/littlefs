cmake_minimum_required(VERSION 3.15)

# Имя компонента
set(UNIT "littlefs")

# Настройки компонента
project(${UNIT} VERSION 1.0.0 LANGUAGES C)
string(TOUPPER ${UNIT} UNIT_UPPER)

set(${PROJECT_NAME}_SOURCES "")
set(${PROJECT_NAME}_INCLUDES "")
set(${PROJECT_NAME}_INTERFACE "")
set(${PROJECT_NAME}_DEFINES "")
set(${PROJECT_NAME}_DEPENDENCIES "")

# add_library(${UNIT} STATIC)

# Добавление исходников
list(APPEND ${PROJECT_NAME}_SOURCES
	${CMAKE_CURRENT_SOURCE_DIR}/lfs.c
	${CMAKE_CURRENT_SOURCE_DIR}/lfs_util.c
)

# Добавление заголовков
list(APPEND ${PROJECT_NAME}_INCLUDES
	${CMAKE_CURRENT_SOURCE_DIR}/
	${${PROJECT_NAME}_DEFINES}
)

# Добавление заголовков интерфейса
list(APPEND ${PROJECT_NAME}_INTERFACE
	${CMAKE_CURRENT_SOURCE_DIR}/
)

# Добавление дефайнов
list(APPEND ${PROJECT_NAME}_DEFINES
)

# todo: add module CMSIS

# Перечисление зависимостей
# list(APPEND ${PROJECT_NAME}_DEPENDENCIES
# 	CMSIS
# 	typedef
# )

# Задание списка компиляции для юнит теста
set(${PROJECT_NAME}_unit_tests_C)

# Установка путей для целей
set(${PROJECT_NAME}_INCLUDES ${CMAKE_CURRENT_SOURCE_DIR}/inc ${${PROJECT_NAME}_INCLUDES})
set(${PROJECT_NAME}_UNIT_TESTS_INCLUDE ${${PROJECT_NAME}_INCLUDES} ${CMAKE_CURRENT_SOURCE_DIR}/unit_tests)

# Настройка автономного запуска компонента
set(${PROJECT_NAME}_standalone_defines GD32F450 TEST)
set(${PROJECT_NAME}_standalone_includes
)
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cmake/unit)

if(PROJECT_IS_TOP_LEVEL)
	add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/cmake/standalone ${CMAKE_CURRENT_SOURCE_DIR}/build/standalone)
endif()
