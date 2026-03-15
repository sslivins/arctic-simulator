# Arctic Heat Pump Simulator
# Runs on M5Stack Atom S3 (ESP32-S3) with RS-485 adapter
cmake_minimum_required(VERSION 3.16)

set(PROJECT_VER "0.8.0")

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(arctic-simulator)
