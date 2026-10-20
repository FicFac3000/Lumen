cmake_minimum_required(VERSION 3.20)
project(lumen CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_library(lumen_core
  src/Uid.cpp
  src/Attribute.cpp
  src/Value.cpp
  src/Patch.cpp
  src/Selection.cpp
  src/Programmer.cpp
  src/Phaser.cpp
  src/Output.cpp
)
target_include_directories(lumen_core PUBLIC include)
target_compile_options(lumen_core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
target_link_libraries(lumen_core PUBLIC Threads::Threads)

enable_testing()

add_executable(test_core tests/test_core.cpp)
target_link_libraries(test_core PRIVATE lumen_core)
target_include_directories(test_core PRIVATE tests)
target_compile_options(test_core PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME core COMMAND test_core)

add_executable(test_programmer tests/test_programmer.cpp)
target_link_libraries(test_programmer PRIVATE lumen_core)
target_include_directories(test_programmer PRIVATE tests)
target_compile_options(test_programmer PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME programmer COMMAND test_programmer)

add_executable(test_phaser tests/test_phaser.cpp)
target_link_libraries(test_phaser PRIVATE lumen_core)
target_include_directories(test_phaser PRIVATE tests)
target_compile_options(test_phaser PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME phaser COMMAND test_phaser)
