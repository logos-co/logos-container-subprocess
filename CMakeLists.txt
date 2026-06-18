cmake_minimum_required(VERSION 3.14)
project(LogosContainerSubprocess LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# Boost.Process/Asio for the process lifecycle and the stdin token pipe.
find_package(Boost REQUIRED COMPONENTS process)

find_package(nlohmann_json REQUIRED)
find_package(spdlog REQUIRED)

# Set output directories
set(CMAKE_ARCHIVE_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_LIBRARY_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/lib)
set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# Build src first
add_subdirectory(src)

# GoogleTest setup
find_package(GTest QUIET)

if(NOT GTest_FOUND)
    message(STATUS "GoogleTest not found via find_package, using FetchContent")
    include(FetchContent)
    FetchContent_Declare(googletest
        URL https://github.com/google/googletest/archive/refs/tags/v1.14.0.zip
    )
    FetchContent_MakeAvailable(googletest)
else()
    message(STATUS "Using system GoogleTest")
endif()

enable_testing()
include(GoogleTest)

# Add tests subdirectory
add_subdirectory(tests)

# Install rules
install(TARGETS logos_container_subprocess
    LIBRARY DESTINATION lib
    ARCHIVE DESTINATION lib
)

# Install headers under include/logos_container_subprocess
install(DIRECTORY src/ DESTINATION include/logos_container_subprocess
    FILES_MATCHING PATTERN "*.h"
)

# Install the generic container-implementation CMake config so consumers can
# find_package(LogosContainerImpl) and get this container (with its own deps)
# without naming it. See cmake/LogosContainerImplConfig.cmake.
install(FILES cmake/LogosContainerImplConfig.cmake
    DESTINATION lib/cmake/LogosContainerImpl
)
