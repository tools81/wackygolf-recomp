cmake_minimum_required(VERSION 3.19)
project(cdi_recomp C)

set(CMAKE_C_STANDARD 11)

# executable name comes from game.json ("exe_name")
file(READ ${CMAKE_SOURCE_DIR}/game.json GAME_JSON)
string(JSON GAME_EXE ERROR_VARIABLE json_err GET ${GAME_JSON} exe_name)
if(json_err)
  set(GAME_EXE cdigame)
endif()

# version from the VERSION file (release tag v<version>)
file(STRINGS ${CMAKE_SOURCE_DIR}/VERSION RECOMP_VERSION LIMIT_COUNT 1)
if(NOT RECOMP_VERSION)
  set(RECOMP_VERSION 0.0.0)
endif()

file(GLOB GEN_SOURCES ${CMAKE_SOURCE_DIR}/gen/*.c)
file(GLOB RT_SOURCES ${CMAKE_SOURCE_DIR}/runtime/*.c)

add_executable(game ${RT_SOURCES} ${GEN_SOURCES})
set_target_properties(game PROPERTIES OUTPUT_NAME ${GAME_EXE})
target_include_directories(game PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
target_compile_definitions(game PRIVATE RECOMP_VERSION="${RECOMP_VERSION}")

if(MSVC)
  target_compile_options(game PRIVATE /O2 /W3 /wd4146 /wd4244 /wd4267 /wd4996 /wd4102 /bigobj)
  target_compile_definitions(game PRIVATE _CRT_SECURE_NO_WARNINGS)
  target_link_libraries(game PRIVATE winmm gdi32 user32)
  set_target_properties(game PROPERTIES WIN32_EXECUTABLE OFF)
else()
  target_compile_options(game PRIVATE -O2 -Wno-unused-label)
endif()

# Differential instruction tester (recompiled semantics vs interpreter).
# Generate test/insn_tests.c with: python tools/gen_tests.py
if(EXISTS ${CMAKE_SOURCE_DIR}/test/insn_tests.c)
  add_executable(insn_test test/harness.c test/insn_tests.c runtime/interp.c runtime/alu.c runtime/disc.c)
  target_include_directories(insn_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen)
  target_compile_definitions(insn_test PRIVATE RECOMP_TEST _CRT_SECURE_NO_WARNINGS)
  if(MSVC)
    target_compile_options(insn_test PRIVATE /O1 /bigobj /wd4146 /wd4244 /wd4267 /wd4996 /wd4102)
  endif()
endif()

# ALU helper check against an independent reference model (python test/gen_alu_vectors.py)
if(EXISTS ${CMAKE_SOURCE_DIR}/test/alu_vectors.h)
  add_executable(alu_test test/alu_test.c runtime/alu.c)
  target_include_directories(alu_test PRIVATE ${CMAKE_SOURCE_DIR}/runtime ${CMAKE_SOURCE_DIR}/gen ${CMAKE_SOURCE_DIR}/test)
endif()
