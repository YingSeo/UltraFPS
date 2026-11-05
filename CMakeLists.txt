cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_VISIBILITY_PRESET hidden)

project(MX350_120FPS_Lock VERSION 2.0.0)

add_library(${PROJECT_NAME} SHARED
    src/main.cpp
)

if (NOT DEFINED ENV{GEODE_SDK})
    message(FATAL_ERROR "Unable to find Geode SDK! Please define GEODE_SDK environment variable")
else()
    message(STATUS "Found Geode: $ENV{GEODE_SDK}")
endif()

add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

if (WIN32)
    target_link_libraries(${PROJECT_NAME} PRIVATE
        opengl32
        winmm
        psapi
        advapi32
        kernel32
    )
    # Tối ưu hóa trình biên dịch MSVC ở mức cao nhất cho CPU x64
    target_compile_options(${PROJECT_NAME} PRIVATE /O2 /Oi /Ot /GL /fp:fast)
    target_link_options(${PROJECT_NAME} PRIVATE /LTCG)
endif()

setup_geode_mod(${PROJECT_NAME})
