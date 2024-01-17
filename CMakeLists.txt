cmake_minimum_required(VERSION 3.15)

project(coz LANGUAGES CXX VERSION 0.0.0)

option(COZ_BUILD_EXAMPLES "Build examples." OFF)

find_package(Boost REQUIRED)

add_library(coz INTERFACE)
target_compile_features(coz INTERFACE cxx_std_20)
target_include_directories(coz INTERFACE include)
target_link_libraries(coz INTERFACE Boost::headers)

install(DIRECTORY include/ TYPE INCLUDE)

if (COZ_BUILD_EXAMPLES)
  add_executable(generator_demo
    example/generator_demo.cpp
  )
  target_link_libraries(generator_demo PUBLIC coz)
endif()
