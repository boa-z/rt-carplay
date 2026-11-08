# SPDX-License-Identifier: GPL-3.0-or-later
cmake_minimum_required(VERSION 3.16)
project(rt_carplay C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

option(RTCP_BUILD_TESTS "Build host-side unit tests" ON)

add_library(rtcp_core STATIC
    core/mfi/mfi2c.c
    core/iap2/iap2_core.c
    core/iap2/iap2_identity.c
    core/iap2/iap2_carplay.c
    core/iap2/iap2_mfi_auth.c
)

target_include_directories(rtcp_core PUBLIC
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/core
    ${CMAKE_CURRENT_SOURCE_DIR}/core/mfi
    ${CMAKE_CURRENT_SOURCE_DIR}/core/iap2/include
)

target_compile_options(rtcp_core PRIVATE -Wall -Wextra -Werror)

if(RTCP_BUILD_TESTS)
    enable_testing()

    foreach(t
            test_mfi2c:${CMAKE_CURRENT_SOURCE_DIR}/tests/test_mfi2c.c
            test_iap2_core:${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_iap2_core.c
            test_iap2_identity:${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_iap2_identity.c
            test_iap2_carplay:${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_iap2_carplay.c
            test_iap2_mfi_auth:${CMAKE_CURRENT_SOURCE_DIR}/tests/unit/test_iap2_mfi_auth.c)
        string(REPLACE ":" ";" pair "${t}")
        list(GET pair 0 name)
        list(GET pair 1 source)
        add_executable(${name} ${source})
        target_link_libraries(${name} PRIVATE rtcp_core)
        target_compile_options(${name} PRIVATE -Wall -Wextra -Werror)
        add_test(NAME ${name} COMMAND ${name})
    endforeach()
endif()
