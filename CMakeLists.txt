cmake_minimum_required(VERSION 3.16)

project(OpenDither VERSION 0.1.0 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

if (WIN32)
    add_executable(opendither WIN32
        src/core.cpp
        src/win32_main.cpp
    )
    target_link_libraries(opendither PRIVATE comdlg32 gdi32 user32 shell32)

    get_filename_component(LLVM_MINGW_BIN "${CMAKE_CXX_COMPILER}" DIRECTORY)
    set(LLVM_MINGW_ROOT "${LLVM_MINGW_BIN}/..")
    add_custom_command(TARGET opendither POST_BUILD
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32/bin/libc++.dll"
            "$<TARGET_FILE_DIR:opendither>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32/bin/libunwind.dll"
            "$<TARGET_FILE_DIR:opendither>"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different
            "${LLVM_MINGW_ROOT}/x86_64-w64-mingw32/bin/libwinpthread-1.dll"
            "$<TARGET_FILE_DIR:opendither>"
    )
else()
    find_package(X11 REQUIRED)
    add_executable(opendither
        src/core.cpp
        src/main.cpp
    )
    target_link_libraries(opendither PRIVATE ${X11_LIBRARIES})
    target_include_directories(opendither PRIVATE ${X11_INCLUDE_DIR})
endif()

if (CMAKE_CXX_COMPILER_ID MATCHES "GNU|Clang")
    target_compile_options(opendither PRIVATE -Wall -Wextra -pedantic)
endif()
