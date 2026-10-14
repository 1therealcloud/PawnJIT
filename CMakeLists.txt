# This Source Code Form is subject to the terms of the Mozilla Public
# License, v. 2.0. If a copy of the MPL was not distributed with this
# file, You can obtain one at https://mozilla.org/MPL/2.0/.
#
# The original code is copyright (c) 2026, 1therealcloud.

cmake_minimum_required(VERSION 3.24)

if(NOT MSVC)
    set(CMAKE_C_FLAGS_INIT "-m32")
    set(CMAKE_CXX_FLAGS_INIT "-m32")
    set(CMAKE_SHARED_LINKER_FLAGS_INIT "-m32")
endif()

project(PawnJIT VERSION 0.54.3 LANGUAGES C CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_POSITION_INDEPENDENT_CODE ON)

if(CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang" AND NOT WIN32)
    add_link_options(-static-libgcc -static-libstdc++)
endif()

set(THIRD_PARTY "${CMAKE_CURRENT_SOURCE_DIR}/third-party")
set(SDK "${THIRD_PARTY}/open.mp-sdk")

add_library(omp-sdk INTERFACE)
target_include_directories(omp-sdk SYSTEM INTERFACE
    "${SDK}/include"
    "${SDK}/lib/glm"
    "${SDK}/lib/robin-hood-hashing/src/include"
    "${SDK}/lib/span-lite/include"
    "${SDK}/lib/string-view-lite/include"
)
target_compile_definitions(omp-sdk INTERFACE
    GLM_FORCE_QUAT_DATA_WXYZ
    GLM_FORCE_SSE2
    NOMINMAX
    nssv_CONFIG_SELECT_STRING_VIEW=nssv_STRING_VIEW_NONSTD
    span_CONFIG_SELECT_SPAN=span_SPAN_NONSTD
)

add_library(subhook STATIC "${THIRD_PARTY}/subhook/subhook.c")
target_include_directories(subhook SYSTEM PUBLIC "${THIRD_PARTY}/subhook")
target_compile_definitions(subhook PRIVATE SUBHOOK_IMPLEMENTATION PUBLIC SUBHOOK_STATIC)

set(ASMJIT_STATIC ON CACHE BOOL "" FORCE)
set(ASMJIT_TEST OFF CACHE BOOL "" FORCE)
add_subdirectory("${THIRD_PARTY}/asmjit" EXCLUDE_FROM_ALL)

add_library(PawnJIT SHARED
    source/main.cpp
    source/decoder.cpp
    source/runtime.cpp
    source/compiler.cpp
)
target_compile_definitions(PawnJIT PRIVATE HAVE_STDINT_H=1 PAWN_CELL_SIZE=32)
target_include_directories(PawnJIT SYSTEM PRIVATE
    "${THIRD_PARTY}/compiler/source"
    "${THIRD_PARTY}/compiler/source/linux"
)
target_link_libraries(PawnJIT PRIVATE omp-sdk subhook asmjit::asmjit)
set_target_properties(PawnJIT PROPERTIES PREFIX "")
