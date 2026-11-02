cmake_minimum_required(VERSION 3.25)

# El nombre del proyecto puede ser cualquiera, pero es recomendable
# que coincida con el nombre de tu mod.
project(InputAssist VERSION 1.0.0)

# Aquí se le dice a CMake qué archivos de código fuente debe compilar.
# Como todo tu mod está en un solo archivo, solo se lista ese.
add_library(InputAssist SHARED
    src/main.cpp
)

# Geode requiere C++23 para compilar correctamente.
target_compile_features(InputAssist PUBLIC cxx_std_23)

# Esta función es la "mágica": se encarga de enlazar el SDK de Geode,
# configurar la plataforma Android y empaquetar todo en el archivo .geode.
setup_geode_mod(InputAssist)
