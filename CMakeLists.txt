# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(rt_carplay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(RTCP_BUILD_TESTS "Build host-side unit tests" ON)

file(GLOB_RECURSE RTCP_CORE_SOURCES CONFIGURE_DEPENDS core/*.c)
add_library(rtcp_core STATIC ${RTCP_CORE_SOURCES})

target_include_directories(rtcp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/mfi
    ${CMAKE_CURRENT_SOURCE_DIR}/core/iap2/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/airplay/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/media/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core/input/include
)

target_compile_options(rtcp_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rtcp_core PUBLIC m)

if(RTCP_BUILD_TESTS)
    enable_testing()

    set(RTCP_TESTS
        test_mfi2c
        test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth
        test_airplay_bplist test_airplay_rtsp_codec test_airplay_tlv8
        test_airplay_screen_codec test_airplay_timing test_airplay_hid_plist
        test_airplay_control_stream test_input_bridge test_media_timestamp
    )

    foreach(name ${RTCP_TESTS})
        if(name STREQUAL "test_mfi2c")
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mfi2c.c)
        else()
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${name}.c)
        endif()
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE rtcp_core)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
