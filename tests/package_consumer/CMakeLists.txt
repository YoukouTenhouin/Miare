cmake_minimum_required(VERSION 3.25)
project(miare_package_consumer LANGUAGES CXX)

find_package(miare CONFIG REQUIRED)
add_executable(miare_package_consumer main.cpp)
target_link_libraries(miare_package_consumer PRIVATE miare::miare)
