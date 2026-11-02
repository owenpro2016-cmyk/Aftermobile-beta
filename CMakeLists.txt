cmake_minimum_required(VERSION 3.25)

project(AftermobileBeta VERSION 1.0.0)

add_library(AftermobileBeta SHARED src/main.cpp)
target_compile_features(AftermobileBeta PUBLIC cxx_std_23)

# 1. Enlazar el SDK de Geode (la línea que faltaba)
add_subdirectory($ENV{GEODE_SDK} ${CMAKE_CURRENT_BINARY_DIR}/geode)

# 2. Configurar el mod
setup_geode_mod(AftermobileBeta)
