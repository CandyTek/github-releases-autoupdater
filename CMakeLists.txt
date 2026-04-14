cmake_minimum_required(VERSION 3.10)

project(github-releases-autoupdater LANGUAGES CXX)

if(NOT WIN32)
    message(FATAL_ERROR "This project is Windows-only.")
endif()

set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

add_library(autoupdater STATIC
    src/cautoupdatergithub.cpp
    src/cautoupdatergithub.h
    src/updateinstaller_win.cpp
    src/updateinstaller.hpp
)

add_library(autoupdater::autoupdater ALIAS autoupdater)

target_include_directories(autoupdater
    PUBLIC
        ${CMAKE_CURRENT_SOURCE_DIR}/src
    PRIVATE
        ${CMAKE_CURRENT_SOURCE_DIR}/3rdparty
)

target_compile_definitions(autoupdater
    PRIVATE
        UNICODE
        _UNICODE
        WIN32_LEAN_AND_MEAN
        NOMINMAX
)

target_link_libraries(autoupdater PRIVATE
    shell32
    winhttp
)

if(MSVC)
    target_compile_options(autoupdater PRIVATE
        /utf-8
        /permissive-
        /MP
        /W4
        /Zc:__cplusplus
    )

    target_link_options(autoupdater PRIVATE
        $<$<CONFIG:Release>:/OPT:REF>
        $<$<CONFIG:Release>:/OPT:ICF>
    )
endif()
