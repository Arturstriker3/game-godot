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

# ---------------------------------------------------------------------------
# opengs_sim — o núcleo da simulação como biblioteca estática C++ pura (A1a).
# STL e o nlohmann vendorizado apenas: NADA de godot-cpp aqui, para que um
# executável headless (CLI) rode o mundo com a mesma ordem do dia. A DLL do
# Godot consome esta biblioteca e só converte tipos na borda (edge/).
# ---------------------------------------------------------------------------
add_library(opengs_sim STATIC
    src/sim/profiling.cpp
    src/sim/sim_core.cpp
    src/sim/sim_day.cpp
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
    src/sim/demography.cpp
    src/sim/strata.cpp
    src/sim/region.cpp
    src/sim/migration.cpp
    src/sim/urban_land.cpp
    src/sim/city_gov.cpp
    src/sim/housing.cpp
    src/sim/scenario_fixture.cpp
    src/sim/commands.cpp
    src/sim/actions.cpp
    src/sim/ai/ai_templates.cpp
    src/sim/ai/ai_view.cpp
    src/sim/ai/ai_private.cpp
    src/sim/ai/ai_group.cpp
    src/sim/ai/ai_government.cpp
    src/sim/ai/ai_phase.cpp
    src/sim/save_value.cpp
    src/sim/save_state.cpp
    src/sim/save_definitions.cpp
)

target_include_directories(opengs_sim PUBLIC "${CMAKE_CURRENT_SOURCE_DIR}/src")

set_target_properties(opengs_sim PROPERTIES
    CXX_EXTENSIONS OFF
    # A estática entra na DLL compartilhada: precisa de PIC.
    POSITION_INDEPENDENT_CODE ON
)

# ---------------------------------------------------------------------------
# economy_gdextension — a DLL do Godot: registra a classe e faz a ponte de
# tipos (edge/). O resto (sim/) vem de opengs_sim.
# ---------------------------------------------------------------------------
add_library(economy_gdextension SHARED
    src/register_types.cpp
    src/economy_sim.cpp
    src/edge/save_load.cpp
    src/edge/value_bridge.cpp
)

target_link_libraries(economy_gdextension PRIVATE godot-cpp opengs_sim)

# Perfil por fase do tick (src/sim/profiling.h): só no build de desenvolvimento.
# Em release OGS_PROFILING não é definido e as macros OGS_SIM_* expandem para
# `((void)0)` — custo zero, nenhum steady_clock::now(), nenhum símbolo.
#
# OGS_PROFILE_RELEASE=ON liga o mesmo perfil num build Release. É um build de
# MEDIÇÃO (A0, docs/plans/perf_pop_financas_plano.md): o release publicado
# continua sem perfil e o perfil adiciona só um relógio por fase por dia.
# Use uma pasta própria (gdextension/build-profile-release) — ela escreve a DLL
# no mesmo nome template_release do build normal.
#
# Vale para os DOIS alvos: o perfil vive no núcleo (opengs_sim) e as macros de
# escopo também aparecem na fachada.
option(OGS_PROFILE_RELEASE "Liga o perfil por fase (OGS_PROFILING) no build Release" OFF)
if(CMAKE_BUILD_TYPE STREQUAL "Debug" OR OGS_PROFILE_RELEASE)
    target_compile_definitions(opengs_sim PRIVATE OGS_PROFILING)
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

# ---------------------------------------------------------------------------
# opengs_sim_cli — executável headless (A1): roda N dias SÓ do núcleo a partir
# de um snapshot JSON e grava o mundo resultante no MESMO formato, para a
# comparação campo a campo contra o replay do Godot
# (tools/world_equivalence_compare.py). Não usa godot-cpp: linka só opengs_sim.
# ---------------------------------------------------------------------------
add_executable(opengs_sim_cli src/cli/main.cpp)

target_link_libraries(opengs_sim_cli PRIVATE opengs_sim)

set_target_properties(opengs_sim_cli PROPERTIES
    CXX_EXTENSIONS OFF
    RUNTIME_OUTPUT_DIRECTORY "${CMAKE_CURRENT_SOURCE_DIR}/bin"
)

# Runtime C++ estático no MinGW: o .exe roda sem libstdc++/libgcc ao lado.
if(MINGW)
    target_link_options(opengs_sim_cli PRIVATE -static -static-libgcc -static-libstdc++)
endif()
