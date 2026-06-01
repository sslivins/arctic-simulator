# Standalone native build for arctic-simulator unit tests.
# Independent of ESP-IDF — uses the host compiler so it runs in CI on
# any Linux/Mac/Windows runner with a C++17 toolchain.
#
# Build & run:
#   cmake -S . -B build -DCMAKE_BUILD_TYPE=Release
#   cmake --build build
#   ctest --test-dir build --output-on-failure
cmake_minimum_required(VERSION 3.16)

project(arctic_simulator_tests CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if(MSVC)
    add_compile_options(/W4 /WX)
else()
    add_compile_options(-Wall -Wextra -Wpedantic -Werror)
endif()

enable_testing()

# ----- tuya_state -----------------------------------------------------------
add_executable(test_tuya_state
    test_tuya_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_codec/tuya_codec.cpp
)
target_include_directories(test_tuya_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME tuya_state COMMAND test_tuya_state)

# ----- tuya_slave -----------------------------------------------------------
add_executable(test_tuya_slave
    test_tuya_slave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_slave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_codec/tuya_codec.cpp
)
target_include_directories(test_tuya_slave PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME tuya_slave COMMAND test_tuya_slave)
