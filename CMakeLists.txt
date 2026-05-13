cmake_minimum_required(VERSION 3.16)

project(OpenDither VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

find_package(X11 REQUIRED)

add_executable(opendither
    src/core.cpp
    src/main.cpp
)

target_link_libraries(opendither PRIVATE ${X11_LIBRARIES})
target_include_directories(opendither PRIVATE ${X11_INCLUDE_DIR})

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(opendither PRIVATE -Wall -Wextra -pedantic)
endif()
