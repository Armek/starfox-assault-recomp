cmake_minimum_required(VERSION 3.16)
project(sfa_recomp C)

# Generated sources come from: python recomp/tools/ppcrecomp.py
set(GENERATED_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/GF7E01/recomp" CACHE PATH "ppcrecomp.py output")
if(NOT EXISTS "${GENERATED_DIR}/sources.txt")
  message(FATAL_ERROR "Run `python recomp/tools/ppcrecomp.py` first (no ${GENERATED_DIR}/sources.txt)")
endif()

file(STRINGS "${GENERATED_DIR}/sources.txt" GENERATED_SOURCES)
# Re-run configure whenever the generator's file list changes.
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${GENERATED_DIR}/sources.txt")
list(TRANSFORM GENERATED_SOURCES PREPEND "${GENERATED_DIR}/")

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
if(NOT CMAKE_BUILD_TYPE)
  set(CMAKE_BUILD_TYPE RelWithDebInfo)
endif()

add_executable(sfa runtime/runtime.c runtime/hle.c runtime/threads.c runtime/dvd.c runtime/gx.c runtime/video.c ${GENERATED_SOURCES})
target_include_directories(sfa PRIVATE runtime "${GENERATED_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  # Generated code has a label per branch target and compares registers to themselves.
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable;-Wno-parentheses-equality;-O1")
  if(NOT WIN32)
    target_link_libraries(sfa PRIVATE m)
  endif()
endif()
find_package(Threads REQUIRED)
target_link_libraries(sfa PRIVATE Threads::Threads)

# SDL2 for the window, OpenGL context and input. On Windows, point SDL2_DIR at the official
# SDL2-devel-*-VC package (default: build/deps/SDL2-2.32.10/cmake); elsewhere use the system SDL2.
if(WIN32 AND NOT SDL2_DIR)
  set(SDL2_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../build/deps/SDL2-2.32.10/cmake")
endif()
find_package(SDL2 REQUIRED CONFIG)
target_link_libraries(sfa PRIVATE SDL2::SDL2)
if(WIN32)
  add_custom_command(TARGET sfa POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:sfa>)
endif()
if(WIN32)
  target_compile_definitions(sfa PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
