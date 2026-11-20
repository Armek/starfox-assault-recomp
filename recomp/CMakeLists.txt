cmake_minimum_required(VERSION 3.16)
# Default to an optimized build. (Must be set before project(): some toolchains, such as clang on
# Windows, otherwise pick Debug and compile the runtime at -O0.)
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
  set(CMAKE_BUILD_TYPE RelWithDebInfo CACHE STRING "Build type" FORCE)
endif()
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

# The replacement system fonts (recomp/fonts, Apache 2.0) are compiled in, so players need only a disc image.
set(FONTS_C "${CMAKE_CURRENT_BINARY_DIR}/fonts_embedded.c")
add_custom_command(OUTPUT "${FONTS_C}"
  COMMAND ${CMAKE_COMMAND} -DIN_DIR=${CMAKE_CURRENT_SOURCE_DIR}/fonts -DOUT=${FONTS_C}
          -P ${CMAKE_CURRENT_SOURCE_DIR}/tools/embed_fonts.cmake
  DEPENDS fonts/font_western.bin fonts/font_japanese.bin tools/embed_fonts.cmake
  COMMENT "Embedding the system fonts")

add_executable(sfa "${FONTS_C}" runtime/runtime.c runtime/hle.c runtime/threads.c runtime/dvd.c runtime/gx.c runtime/video.c runtime/card.c runtime/ax.c runtime/launcher.c runtime/updater.c ${GENERATED_SOURCES})
# The release version (recomp/VERSION, bumped for each release): shown on the start screen and compared
# with GitHub's releases by the updater.
file(STRINGS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION" SFA_VERSION LIMIT_COUNT 1)
string(STRIP "${SFA_VERSION}" SFA_VERSION)  # a checkout may have CRLF line endings
set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/VERSION")
target_compile_definitions(sfa PRIVATE SFA_VERSION="${SFA_VERSION}")
target_include_directories(sfa PRIVATE runtime "${GENERATED_DIR}")

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  # Generated code has a label per branch target and compares registers to themselves.
  # GENERATED_OPT: optimization level for the generated code (-O1 builds faster; -O2 runs faster)
  set(GENERATED_OPT "-O1" CACHE STRING "Optimization flag for the generated sources")
  set_source_files_properties(${GENERATED_SOURCES} PROPERTIES
    COMPILE_OPTIONS "-Wno-unused-label;-Wno-unused-variable;-Wno-parentheses-equality;${GENERATED_OPT}")
  if(NOT WIN32)
    target_link_libraries(sfa PRIVATE m)
  endif()
endif()
# -DRT_WATCH=ON: debug build with a memory write watchpoint (SFA_WATCH=<addr>:<len>)
option(RT_WATCH "Compile the guest memory write watchpoint" OFF)
if(RT_WATCH)
  target_compile_definitions(sfa PRIVATE RT_WATCH)
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
  target_link_libraries(sfa PRIVATE winmm comdlg32 ole32 dbghelp)  # timeBeginPeriod (1 ms sleeps); the Open dialog
endif()
if(WIN32)
  add_custom_command(TARGET sfa POST_BUILD COMMAND ${CMAKE_COMMAND} -E copy_if_different
    $<TARGET_FILE:SDL2::SDL2> $<TARGET_FILE_DIR:sfa>)
endif()
if(WIN32)
  target_compile_definitions(sfa PRIVATE _CRT_SECURE_NO_WARNINGS)
endif()
