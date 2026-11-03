cmake_minimum_required(VERSION 3.20)
project(jazz VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Jazz requires Linux and rdma-core; use the Docker build environment.")
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(JAZZ_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(JAZZ_BUILD_PROTOTYPE "Build the historical synchronous prototype and regression tests" ON)
find_package(PkgConfig REQUIRED)
pkg_check_modules(VERBS REQUIRED IMPORTED_TARGET libibverbs)
pkg_check_modules(RDMACM REQUIRED IMPORTED_TARGET librdmacm)

add_compile_definitions(_GNU_SOURCE)
add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
                    -Wstrict-prototypes -Wmissing-prototypes)
if(JAZZ_SANITIZE)
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# 新核心独立构建；不调用旧 connection 的资源管理。
add_library(jazz STATIC src/runtime/engine.c src/transport/rdma/device.c)
target_include_directories(jazz PUBLIC include PRIVATE src)
target_link_libraries(jazz PRIVATE PkgConfig::VERBS PkgConfig::RDMACM)

include(CTest)
if(BUILD_TESTING)
    foreach(name engine engine_rdma)
        add_executable(test_${name} tests/unit/test_${name}.c)
        target_include_directories(test_${name} PRIVATE src tests/unit)
        target_link_libraries(test_${name} PRIVATE jazz)
        add_test(NAME ${name} COMMAND test_${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endforeach()
endif()

if(JAZZ_BUILD_PROTOTYPE)
    # 旧同步原型仅用于对照回归，不链接进新核心。
    add_library(jazz_prototype STATIC src/core/wire.c src/core/socket.c src/rdma/connection.c
                           src/rdma/cm.c src/rdma/oob.c src/rdma/transfer.c)
    target_include_directories(jazz_prototype PUBLIC include PRIVATE src)
    target_link_libraries(jazz_prototype PRIVATE PkgConfig::VERBS PkgConfig::RDMACM)
    add_executable(jazz_roundtrip examples/roundtrip.c)
    target_link_libraries(jazz_roundtrip PRIVATE jazz_prototype)

    if(BUILD_TESTING)
        foreach(name wire socket api)
            add_executable(test_${name} tests/unit/test_${name}.c)
            target_include_directories(test_${name} PRIVATE src)
            target_link_libraries(test_${name} PRIVATE jazz_prototype)
            add_test(NAME ${name} COMMAND test_${name})
            set_tests_properties(${name} PROPERTIES TIMEOUT 15)
        endforeach()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME rdma_roundtrip COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/roundtrip.py
                 --binary $<TARGET_FILE:jazz_roundtrip>)
        set_tests_properties(rdma_roundtrip PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
    endif()
endif()
