cmake_minimum_required(VERSION 3.20)
project(jz VERSION 0.1.0 LANGUAGES C)

# The build requires Linux RDMA userspace APIs; use Docker on macOS.
# A container does not automatically provide RDMA devices or kernel support.
if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Linux and rdma-core are required; use the Docker build environment.")
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
# Use C17 syntax; _GNU_SOURCE below controls libc declarations.
set(CMAKE_C_EXTENSIONS OFF)
option(JZ_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
option(JZ_BUILD_PROTOTYPE "Build the historical synchronous prototype and regression tests" ON)
find_package(PkgConfig REQUIRED)
# Use package metadata rather than hard-coded library paths.
# libibverbs provides device/data operations; librdmacm provides CM events.
pkg_check_modules(VERBS REQUIRED IMPORTED_TARGET libibverbs)
pkg_check_modules(RDMACM REQUIRED IMPORTED_TARGET librdmacm)

add_compile_definitions(_GNU_SOURCE)
# Treat suspicious conversions and declarations as build failures, not runtime checks.
add_compile_options(-Wall -Wextra -Wpedantic -Werror -Wconversion -Wshadow
                    -Wstrict-prototypes -Wmissing-prototypes)
if(JZ_SANITIZE)
    # ASan detects CPU memory errors; UBSan detects some undefined behavior.
    # They cannot observe RNIC DMA or replace real RDMA testing.
    add_compile_options(-fsanitize=address,undefined -fno-omit-frame-pointer)
    add_link_options(-fsanitize=address,undefined)
endif()

# Build the new resource core independently of prototype connection management.
add_library(jz STATIC src/runtime/engine.c src/transport/rdma/device.c)
target_include_directories(jz PUBLIC include PRIVATE src)
target_link_libraries(jz PRIVATE PkgConfig::VERBS PkgConfig::RDMACM)

include(CTest)
if(BUILD_TESTING)
    foreach(name engine engine_rdma)
        add_executable(test_${name} tests/unit/test_${name}.c)
        target_include_directories(test_${name} PRIVATE src tests/unit)
        target_link_libraries(test_${name} PRIVATE jz)
        add_test(NAME ${name} COMMAND test_${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 30 SKIP_RETURN_CODE 77)
    endforeach()
endif()

if(JZ_BUILD_PROTOTYPE)
    # Keep the synchronous prototype isolated for regression comparisons.
    add_library(jz_prototype STATIC src/core/wire.c src/core/socket.c src/rdma/connection.c
                           src/rdma/cm.c src/rdma/oob.c src/rdma/transfer.c)
    # The prototype public header hides internal verbs layouts.
    target_include_directories(jz_prototype PUBLIC include PRIVATE src)
    target_link_libraries(jz_prototype PRIVATE PkgConfig::VERBS PkgConfig::RDMACM)
    # The standalone example calls the prototype public API.
    add_executable(jz_roundtrip examples/roundtrip.c)
    target_link_libraries(jz_roundtrip PRIVATE jz_prototype)

    if(BUILD_TESTING)
        # Wire, socket and API tests need no RDMA device.
        foreach(name wire socket api)
            add_executable(test_${name} tests/unit/test_${name}.c)
            target_include_directories(test_${name} PRIVATE src)
            target_link_libraries(test_${name} PRIVATE jz_prototype)
            add_test(NAME ${name} COMMAND test_${name})
            set_tests_properties(${name} PROPERTIES TIMEOUT 15)
        endforeach()
        find_package(Python3 REQUIRED COMPONENTS Interpreter)
        add_test(NAME rdma_roundtrip COMMAND ${Python3_EXECUTABLE}
                 ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/roundtrip.py
                 --binary $<TARGET_FILE:jz_roundtrip>)
        # Without explicit JZ_TEST_IP, the script returns 77 and CTest reports SKIP.
        # Once device tests are enabled, failures must not be hidden as skips.
        # TIMEOUT is the outer test watchdog, not the library operation deadline.
        set_tests_properties(rdma_roundtrip PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
    endif()
endif()
