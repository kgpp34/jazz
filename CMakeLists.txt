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
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
find_package(PkgConfig REQUIRED)
find_package(Threads REQUIRED)
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

# Keep generic lifecycle compilation independent of RDMA headers and libraries.
add_library(jz_runtime OBJECT src/runtime/engine.c src/memory/interval.c src/memory/registry.c)
target_link_libraries(jz_runtime PUBLIC Threads::Threads)
target_include_directories(jz_runtime PUBLIC include PRIVATE src)
add_library(jz STATIC $<TARGET_OBJECTS:jz_runtime>
    src/transport/rdma/backend.c src/transport/rdma/device.c
    src/transport/rdma/context_source.c src/transport/rdma/driver.c
    src/transport/rdma/registration.c src/platform/host_memory.c)
target_include_directories(jz PUBLIC include PRIVATE src)
target_link_libraries(jz PRIVATE PkgConfig::VERBS PkgConfig::RDMACM PUBLIC Threads::Threads)

include(CTest)
if(BUILD_TESTING)
    add_executable(test_engine tests/unit/test_engine.c $<TARGET_OBJECTS:jz_runtime>)
    target_include_directories(test_engine PRIVATE include src tests/unit)
    target_link_libraries(test_engine PRIVATE Threads::Threads)
    add_test(NAME engine COMMAND test_engine)
    add_executable(test_memory tests/unit/test_memory.c $<TARGET_OBJECTS:jz_runtime>)
    target_include_directories(test_memory PRIVATE include src tests/unit)
    target_link_libraries(test_memory PRIVATE Threads::Threads)
    add_test(NAME memory COMMAND test_memory)
    add_executable(test_host_memory tests/unit/test_host_memory.c)
    target_include_directories(test_host_memory PRIVATE src tests/unit)
    target_link_libraries(test_host_memory PRIVATE jz)
    add_test(NAME host_memory COMMAND test_host_memory)
    add_executable(test_rdma tests/unit/test_rdma.c)
    target_include_directories(test_rdma PRIVATE src tests/unit)
    target_link_libraries(test_rdma PRIVATE jz)
    add_test(NAME rdma COMMAND test_rdma)
    add_executable(test_engine_rdma tests/integration/engine_rdma.c)
    target_include_directories(test_engine_rdma PRIVATE src tests/unit)
    target_link_libraries(test_engine_rdma PRIVATE jz)
    add_test(NAME engine_rdma COMMAND test_engine_rdma)
    set_tests_properties(engine rdma engine_rdma PROPERTIES TIMEOUT 30)
    set_tests_properties(engine_rdma PROPERTIES SKIP_RETURN_CODE 77)
endif()
