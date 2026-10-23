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
  src/Xml.cpp
  src/Zip.cpp
  src/Gdtf.cpp
  src/FixtureEditor.cpp
  src/Library.cpp
  src/Binding.cpp
  src/Undo.cpp
  src/Mvr.cpp
  src/Output.cpp
  src/Input.cpp
  src/Timecode.cpp
  src/Laser.cpp
  src/Simulated.cpp
    src/Surface.cpp
    src/SurfaceView.cpp
)
target_include_directories(lumen_core PUBLIC include)
target_compile_options(lumen_core PRIVATE -Wall -Wextra -Wpedantic)

find_package(Threads REQUIRED)
find_package(SQLite3 REQUIRED)
find_package(EXPAT REQUIRED)
find_package(ZLIB REQUIRED)
target_link_libraries(lumen_core PUBLIC Threads::Threads SQLite::SQLite3
                      EXPAT::EXPAT ZLIB::ZLIB)

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

add_executable(test_gdtf tests/test_gdtf.cpp)
target_link_libraries(test_gdtf PRIVATE lumen_core)
target_include_directories(test_gdtf PRIVATE tests)
target_compile_options(test_gdtf PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME gdtf COMMAND test_gdtf)

add_executable(test_mvr tests/test_mvr.cpp)
target_link_libraries(test_mvr PRIVATE lumen_core)
target_include_directories(test_mvr PRIVATE tests)
target_compile_options(test_mvr PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME mvr COMMAND test_mvr)

add_executable(test_editor tests/test_editor.cpp)
target_link_libraries(test_editor PRIVATE lumen_core)
target_include_directories(test_editor PRIVATE tests)
target_compile_options(test_editor PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME editor COMMAND test_editor)

add_executable(test_library tests/test_library.cpp)
target_link_libraries(test_library PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_library PRIVATE tests)
target_compile_options(test_library PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME library COMMAND test_library)

add_executable(test_binding tests/test_binding.cpp)
target_link_libraries(test_binding PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_binding PRIVATE tests)
target_compile_options(test_binding PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME binding COMMAND test_binding)

add_executable(test_undo tests/test_undo.cpp)
target_link_libraries(test_undo PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_undo PRIVATE tests)
target_compile_options(test_undo PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME undo COMMAND test_undo)

add_executable(test_input tests/test_input.cpp)
target_link_libraries(test_input PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_input PRIVATE tests)
target_compile_options(test_input PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME input COMMAND test_input)

add_executable(test_laser tests/test_laser.cpp)
target_link_libraries(test_laser PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_laser PRIVATE tests)
target_compile_options(test_laser PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME laser COMMAND test_laser)

add_executable(test_surface tests/test_surface.cpp)
target_link_libraries(test_surface PRIVATE lumen_core SQLite::SQLite3)
target_include_directories(test_surface PRIVATE tests)
target_compile_options(test_surface PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME surface COMMAND test_surface)



# ---------------------------------------------------------------------------
# Applications
# ---------------------------------------------------------------------------
# lumen-server: the engine with its interface in a browser. Everything that
# needs hardware is simulated, so it runs anywhere.
add_executable(lumen-server
  apps/server/main.cpp
  apps/server/Http.cpp
  apps/server/Json.cpp
)
target_link_libraries(lumen-server PRIVATE lumen_core)
target_include_directories(lumen-server PRIVATE apps/server)
target_compile_options(lumen-server PRIVATE -Wall -Wextra -Wpedantic)
target_compile_definitions(lumen-server PRIVATE
  LUMEN_WEB_ROOT="${CMAKE_CURRENT_SOURCE_DIR}/web")

# lumen-cli: the same engine, driven from the terminal.
add_executable(lumen-cli apps/cli/main.cpp)
target_link_libraries(lumen-cli PRIVATE lumen_core)
target_compile_options(lumen-cli PRIVATE -Wall -Wextra -Wpedantic)

add_executable(test_server tests/test_server.cpp apps/server/Http.cpp apps/server/Json.cpp)
target_link_libraries(test_server PRIVATE lumen_core)
target_include_directories(test_server PRIVATE tests apps/server)
target_compile_options(test_server PRIVATE -Wall -Wextra -Wpedantic)
add_test(NAME server COMMAND test_server)
