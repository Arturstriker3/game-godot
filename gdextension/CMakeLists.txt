cmake_minimum_required(VERSION 3.25)

project(economy_gdextension LANGUAGES CXX)

# Evita que o Godot tente importar os .obj/.a da pasta de build como recursos.
file(WRITE "${CMAKE_CURRENT_BINARY_DIR}/.gdignore" "")

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

# godot-cpp: use -DGODOTCPP_DIR ou a variável de ambiente GODOTCPP_DIR.
if(NOT DEFINED GODOTCPP_DIR)
    if(DEFINED ENV{GODOTCPP_DIR})
        set(GODOTCPP_DIR "$ENV{GODOTCPP_DIR}" CACHE PATH "Caminho do godot-cpp")
    else()
        message(FATAL_ERROR "Defina -DGODOTCPP_DIR=<pasta do godot-cpp> ou a variável de ambiente GODOTCPP_DIR.")
    endif()
endif()
set(GODOTCPP_API_VERSION "4.7" CACHE STRING "Versão da API do Godot alvo")
add_subdirectory(
    "${GODOTCPP_DIR}"
    "${CMAKE_CURRENT_BINARY_DIR}/godot-cpp"
    EXCLUDE_FROM_ALL
)

add_library(economy_gdextension SHARED
    src/register_types.cpp
    src/economy_sim.cpp
    src/sim/profiling.cpp
    src/sim/sim_core.cpp
    src/sim/freight.cpp
    src/sim/logistics.cpp
    src/sim/trade.cpp
    src/sim/production.cpp
    src/sim/prices.cpp
    src/sim/consumption.cpp
    src/sim/labor.cpp
    src/sim/services.cpp
    src/sim/fiscal.cpp
    src/sim/fx.cpp
    src/sim/investment.cpp
    src/sim/construction.cpp
    src/sim/urban_land.cpp
    src/sim/city_gov.cpp
    src/sim/housing.cpp
    src/sim/scenario_fixture.cpp
    src/edge/save_load.cpp
)

target_link_libraries(economy_gdextension PRIVATE godot-cpp)

# Perfil por fase do tick (src/sim/profiling.h): só no build de desenvolvimento.
# Em release OGS_PROFILING não é definido e as macros OGS_SIM_* expandem para
# `((void)0)` — custo zero, nenhum steady_clock::now(), nenhum símbolo.
#
# OGS_PROFILE_RELEASE=ON liga o mesmo perfil num build Release. É um build de
# MEDIÇÃO (A0, docs/plans/perf_pop_financas_plano.md): o release publicado
# continua sem perfil e o perfil adiciona só um relógio por fase por dia.
# Use uma pasta própria (gdextension/build-profile-release) — ela escreve a DLL
# no mesmo nome template_release do build normal.
option(OGS_PROFILE_RELEASE "Liga o perfil por fase (OGS_PROFILING) no build Release" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR OGS_PROFILE_RELEASE)
    target_compile_definitions(economy_gdextension PRIVATE OGS_PROFILING)
endif()

get_target_property(WE_GODOTCPP_PLATFORM godot-cpp GODOTCPP_PLATFORM)
get_target_property(WE_GODOTCPP_TARGET godot-cpp GODOTCPP_TARGET)
get_target_property(WE_GODOTCPP_ARCH godot-cpp GODOTCPP_ARCH)

set_target_properties(economy_gdextension PROPERTIES
    CXX_EXTENSIONS OFF
    CXX_VISIBILITY_PRESET hidden
    VISIBILITY_INLINES_HIDDEN ON
    PREFIX ""
    OUTPUT_NAME "economy_gdextension.${WE_GODOTCPP_PLATFORM}.${WE_GODOTCPP_TARGET}.${WE_GODOTCPP_ARCH}"
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
    LIBRARY_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

# Runtime C++ estático no MinGW: a DLL não exige libstdc++/libgcc ao lado.
if(MINGW)
    target_link_options(economy_gdextension PRIVATE -static -static-libgcc -static-libstdc++)
endif()
