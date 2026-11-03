cmake_minimum_required(VERSION 3.20)
project(jazz VERSION 0.1.0 LANGUAGES C)

if(NOT CMAKE_SYSTEM_NAME STREQUAL "Linux")
    message(FATAL_ERROR "Jazz requires Linux and rdma-core; use the Docker build environment.")
endif()
set(CMAKE_C_STANDARD 17)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
option(JAZZ_SANITIZE "Enable address and undefined behavior sanitizers" OFF)
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

add_library(jazz STATIC src/core/wire.c src/core/socket.c src/rdma/connection.c
                       src/rdma/cm.c src/rdma/oob.c src/rdma/transfer.c)
target_include_directories(jazz PUBLIC include PRIVATE src)
target_link_libraries(jazz PRIVATE PkgConfig::VERBS PkgConfig::RDMACM)
add_executable(jazz_roundtrip examples/roundtrip.c)
target_link_libraries(jazz_roundtrip PRIVATE jazz)

include(CTest)
if(BUILD_TESTING)
    foreach(name wire socket api)
        add_executable(test_${name} tests/unit/test_${name}.c)
        target_include_directories(test_${name} PRIVATE src)
        target_link_libraries(test_${name} PRIVATE jazz)
        add_test(NAME ${name} COMMAND test_${name})
        set_tests_properties(${name} PROPERTIES TIMEOUT 15)
    endforeach()
    find_package(Python3 REQUIRED COMPONENTS Interpreter)
    add_test(NAME rdma_roundtrip COMMAND ${Python3_EXECUTABLE}
             ${CMAKE_CURRENT_SOURCE_DIR}/tests/integration/roundtrip.py
             --binary $<TARGET_FILE:jazz_roundtrip>)
    set_tests_properties(rdma_roundtrip PROPERTIES TIMEOUT 150 SKIP_RETURN_CODE 77)
endif()
