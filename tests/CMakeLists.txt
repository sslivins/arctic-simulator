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

# The arctic-macon library (the single source of register/field truth) builds
# natively as the static library `arctic_macon`.
add_subdirectory(${CMAKE_CURRENT_SOURCE_DIR}/../components/arctic-macon arctic-macon)

# ----- tuya_state -----------------------------------------------------------
add_executable(test_tuya_state
    test_tuya_state.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
)
target_link_libraries(test_tuya_state PRIVATE arctic_macon)
target_include_directories(test_tuya_state PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME tuya_state COMMAND test_tuya_state)

# ----- tuya_slave -----------------------------------------------------------
add_executable(test_tuya_slave
    test_tuya_slave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_slave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
)
target_link_libraries(test_tuya_slave PRIVATE arctic_macon)
target_include_directories(test_tuya_slave PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME tuya_slave COMMAND test_tuya_slave)

# ----- tuya_slave golden tests against real captures -----------------------
add_executable(test_tuya_slave_golden
    test_tuya_slave_golden.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_slave.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
)
target_link_libraries(test_tuya_slave_golden PRIVATE arctic_macon)
target_include_directories(test_tuya_slave_golden PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)
target_compile_definitions(test_tuya_slave_golden PRIVATE
    CAPTURE_DATA_DIR="${CMAKE_CURRENT_SOURCE_DIR}/data"
)

add_test(NAME tuya_slave_golden COMMAND test_tuya_slave_golden)

# ----- playback_raw: sniffer raw-jsonl decoder ----------------------------
add_executable(test_playback_raw
    test_playback_raw.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/playback_raw.cpp
)
target_link_libraries(test_playback_raw PRIVATE arctic_macon)
target_include_directories(test_playback_raw PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME playback_raw COMMAND test_playback_raw)

# ----- presets + semantic state (library decode) --------------------------
add_executable(test_presets_semantic
    test_presets_semantic.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/register_map.cpp
    ${CMAKE_CURRENT_SOURCE_DIR}/../main/tuya_state.cpp
)
target_link_libraries(test_presets_semantic PRIVATE arctic_macon)
target_include_directories(test_presets_semantic PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/../main
)

add_test(NAME presets_semantic COMMAND test_presets_semantic)
