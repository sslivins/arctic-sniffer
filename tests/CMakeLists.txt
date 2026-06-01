# Standalone native build for tuya_codec unit tests.
# Independent of ESP-IDF — uses the host compiler so it runs in CI on
# any Linux/Mac/Windows runner with a C++17 toolchain.
#
# Build & run:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)

project(arctic_sniffer_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

add_executable(test_tuya_codec
    test_tuya_codec.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_codec.cpp
)
target_include_directories(test_tuya_codec PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME tuya_codec COMMAND test_tuya_codec)
