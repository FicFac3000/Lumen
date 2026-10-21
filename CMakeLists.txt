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
  src/Cue.cpp
  src/Engine.cpp
  src/Command.cpp
  src/Builtins.cpp
  src/Show.cpp
  src/Layout.cpp
  src/Radial.cpp
  src/Preset.cpp
  src/Recipe.cpp
  src/Group.cpp
  src/Output.cpp
)
target_include_directories(lumen_core PUBLIC include)
target_compile_options(lumen_core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
target_link_libraries(lumen_core PUBLIC Threads::Threads SQLite::SQLite3)

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

add_executable(test_cue tests/test_cue.cpp)
target_link_libraries(test_cue PRIVATE lumen_core)
target_include_directories(test_cue PRIVATE tests)
target_compile_options(test_cue PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME cue COMMAND test_cue)

add_executable(test_engine tests/test_engine.cpp)
target_link_libraries(test_engine PRIVATE lumen_core)
target_include_directories(test_engine PRIVATE tests)
target_compile_options(test_engine PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME engine COMMAND test_engine)

add_executable(test_command tests/test_command.cpp)
target_link_libraries(test_command PRIVATE lumen_core)
target_include_directories(test_command PRIVATE tests)
target_compile_options(test_command PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME command COMMAND test_command)

add_executable(test_show tests/test_show.cpp)
target_link_libraries(test_show PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_show PRIVATE tests)
target_compile_options(test_show PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME show COMMAND test_show)

add_executable(test_ui tests/test_ui.cpp)
target_link_libraries(test_ui PRIVATE lumen_core)
target_include_directories(test_ui PRIVATE tests)
target_compile_options(test_ui PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME ui COMMAND test_ui)

add_executable(test_preset tests/test_preset.cpp)
target_link_libraries(test_preset PRIVATE lumen_core)
target_include_directories(test_preset PRIVATE tests)
target_compile_options(test_preset PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME preset COMMAND test_preset)

add_executable(test_recipe tests/test_recipe.cpp)
target_link_libraries(test_recipe PRIVATE lumen_core)
target_include_directories(test_recipe PRIVATE tests)
target_compile_options(test_recipe PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME recipe COMMAND test_recipe)

add_executable(test_group tests/test_group.cpp)
target_link_libraries(test_group PRIVATE lumen_core)
target_include_directories(test_group PRIVATE tests)
target_compile_options(test_group PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME group COMMAND test_group)


