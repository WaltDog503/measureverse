cmake_minimum_required(VERSION 3.16)
project(measureverse LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)  # for VS Code IntelliSense / clangd

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE Release)
endif()

find_package(SQLite3 REQUIRED)

add_library(mv_core STATIC src/sizing.cpp)
target_include_directories(mv_core PUBLIC src)
target_compile_options(mv_core PRIVATE -Wall -Wextra -Wpedantic)

add_executable(measureverse_server src/measureverse_server.cpp)
target_link_libraries(measureverse_server PRIVATE mv_core SQLite::SQLite3)
target_compile_options(measureverse_server PRIVATE -Wall -Wextra -Wpedantic)

enable_testing()
add_executable(test_measureverse tests/test_measureverse.cpp)
target_link_libraries(test_measureverse PRIVATE mv_core)
target_compile_options(test_measureverse PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME measureverse_tests COMMAND test_measureverse)

# Copy the dashboard next to the binary so `./build/measureverse_server` serves it.
add_custom_command(TARGET measureverse_server POST_BUILD
  COMMAND ${CMAKE_COMMAND} -E copy_if_different
          ${CMAKE_SOURCE_DIR}/index.html $<TARGET_FILE_DIR:measureverse_server>/index.html)
