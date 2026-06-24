cmake_minimum_required(VERSION 3.15)
project(student-record-system CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# Default to Release if no build type was given.
if(NOT CMAKE_BUILD_TYPE AND NOT CMAKE_CONFIGURATION_TYPES)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

if(MSVC)
    add_compile_options(/W4 /permissive- /utf-8)
    # MSVC: silence unsafe-CRT warnings for cross-platform code.
    add_compile_definitions(_CRT_SECURE_NO_WARNINGS)
else()
    add_compile_options(-Wall -Wextra -Wpedantic)
endif()

# Threads (pthread on POSIX; on Windows the std::thread runtime is bundled).
find_package(Threads REQUIRED)

# Common include path
include_directories(${CMAKE_SOURCE_DIR})

# --- Server ---
add_executable(student_server server/main.cpp)
target_link_libraries(student_server PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(student_server PRIVATE ws2_32)
endif()

# --- Client ---
add_executable(student_client client/main.cpp)
target_link_libraries(student_client PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(student_client PRIVATE ws2_32)
endif()

# --- Tests ---
enable_testing()
add_executable(student_tests tests/test_main.cpp)
target_link_libraries(student_tests PRIVATE Threads::Threads)
if(WIN32)
    target_link_libraries(student_tests PRIVATE ws2_32)
endif()
add_test(NAME unit_tests COMMAND student_tests)

# Put all binaries in bin/ to match the Makefile layout.
set_target_properties(student_server student_client student_tests PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}/bin"
)
