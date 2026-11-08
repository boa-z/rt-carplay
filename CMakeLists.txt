# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(rt_carplay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(RTCP_BUILD_TESTS "Build host-side unit tests" ON)

# mbedTLS source tree (2.28.x).  Defaults to third_party/mbedtls populated
# by scripts/fetch-mbedtls.sh; can be overridden with -DRTCP_MBEDTLS_DIR=...
set(RTCP_MBEDTLS_DIR "${CMAKE_CURRENT_SOURCE_DIR}/third_party/mbedtls"
    CACHE PATH "mbedTLS 2.28 source tree")
set(RTCP_MBEDTLS_LIB "${CMAKE_CURRENT_SOURCE_DIR}/build/mbedtls/libmbedcrypto.a")
add_custom_command(
    OUTPUT "${RTCP_MBEDTLS_LIB}"
    COMMAND "${CMAKE_CURRENT_SOURCE_DIR}/scripts/build-mbedtls-host.sh" "${RTCP_MBEDTLS_DIR}"
    COMMENT "Building host libmbedcrypto"
    VERBATIM)
add_custom_target(rtcp_mbedtls DEPENDS "${RTCP_MBEDTLS_LIB}")

add_library(rtcp_mbedcrypto STATIC IMPORTED GLOBAL)
set_target_properties(rtcp_mbedcrypto PROPERTIES
    IMPORTED_LOCATION "${RTCP_MBEDTLS_LIB}"
    INTERFACE_INCLUDE_DIRECTORIES "${RTCP_MBEDTLS_DIR}/include")
add_dependencies(rtcp_mbedcrypto rtcp_mbedtls)

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
    ${CMAKE_CURRENT_SOURCE_DIR}/core/discovery/include
    ${RTCP_MBEDTLS_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/config
    ${CMAKE_CURRENT_SOURCE_DIR}/tests/support
)

target_compile_definitions(rtcp_core PUBLIC
    MBEDTLS_CONFIG_FILE="mbedtls_rtcp_host.h")
target_compile_options(rtcp_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(rtcp_core PUBLIC rtcp_mbedcrypto m)

file(GLOB_RECURSE RTCP_PORT_LINUX_SOURCES CONFIGURE_DEPENDS port/linux/*.c)
add_library(rtcp_port_linux STATIC ${RTCP_PORT_LINUX_SOURCES})
target_include_directories(rtcp_port_linux PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/port/linux/include)
find_package(Threads REQUIRED)
target_link_libraries(rtcp_port_linux PUBLIC Threads::Threads)

if(RTCP_BUILD_TESTS)
    enable_testing()

    set(RTCP_TESTS
        test_mfi2c
        test_iap2_core test_iap2_identity test_iap2_carplay test_iap2_mfi_auth
        test_iap2_parameter test_iap2_session
        test_airplay_bplist test_airplay_info_plist test_airplay_rtsp_codec test_airplay_tlv8
        test_airplay_screen_codec test_airplay_timing test_airplay_hid_plist
        test_airplay_control_stream test_airplay_screen_stream test_airplay_mfi_sap
        test_airplay_srp6a test_airplay_pair_setup test_airplay_pair_verify
        test_airplay_rtsp_pairing test_airplay_rtsp_session
        test_airplay_pairing_store
        test_airplay_control_cipher test_airplay_control_port
        test_airplay_timing_port test_airplay_event_port
        test_airplay_screen_server
        test_discovery test_mdns_responder test_port_linux test_input_bridge
        test_media_timestamp
    )

    set(RTCP_PORT_TESTS
        test_port_linux test_airplay_control_port test_airplay_timing_port
        test_airplay_event_port test_airplay_screen_server)

    foreach(name ${RTCP_TESTS})
        if(name STREQUAL "test_mfi2c")
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mfi2c.c)
        else()
            set(source ${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/${name}.c)
        endif()
        add_executable(${name} ${source})
        if(name STREQUAL "test_port_linux")
            target_link_libraries(${name} PRIVATE rtcp_port_linux)
        elseif(name IN_LIST RTCP_PORT_TESTS)
            target_link_libraries(${name} PRIVATE rtcp_core rtcp_port_linux)
        else()
            target_link_libraries(${name} PRIVATE rtcp_core)
        endif()
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
