# SPDX-License-Identifier: GPL-3.0-or-later
# Copyright (C) 2026 KL5NE, Alaska HamRadio, and AHR-Decoder contributors
# Vendored subset of AHR-Decoder for Nexus: the portable decoder core, its
# plain C API and (tests only) the synthetic rig-audio generator. The full
# project (SDRangel plugin, test suites, tools) lives upstream; see README.md.
cmake_minimum_required(VERSION 3.16)
project(ahr_decoder_vendored CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

add_library(ahrwwv_core STATIC core/src/timecode.cpp core/src/decoder.cpp)
target_include_directories(ahrwwv_core PUBLIC core/include)
set_target_properties(ahrwwv_core PROPERTIES POSITION_INDEPENDENT_CODE ON)

add_library(ahrwwv_capi STATIC capi/src/ahr_wwv_capi.cpp)
target_include_directories(ahrwwv_capi PUBLIC capi/include)
target_link_libraries(ahrwwv_capi PUBLIC ahrwwv_core)
set_target_properties(ahrwwv_capi PROPERTIES POSITION_INDEPENDENT_CODE ON)

option(AHRWWV_CAPI_TEST_SYNTH "Build the test-only synthetic rig-audio library" OFF)
if(AHRWWV_CAPI_TEST_SYNTH)
    add_library(ahrwwv_testsynth STATIC capi/testsynth/ahr_wwv_test_synth.cpp sim/wwvsim.cpp)
    target_include_directories(ahrwwv_testsynth PUBLIC capi/testsynth sim)
    target_link_libraries(ahrwwv_testsynth PUBLIC ahrwwv_core)
    set_target_properties(ahrwwv_testsynth PROPERTIES POSITION_INDEPENDENT_CODE ON)
endif()
