cmake_minimum_required(VERSION 3.21)
set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT GEODE_SDK)
    if(DEFINED ENV{GEODE_SDK})
        set(GEODE_SDK $ENV{GEODE_SDK})
    endif()
endif()

include(${GEODE_SDK}/cmake/Geode.cmake)

project(AftermobileBeta VERSION 1.0.0)

add_library(AftermobileBeta SHARED src/main.cpp)
setup_geode_mod(AftermobileBeta)
