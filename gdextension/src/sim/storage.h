#pragma once
// G2 — ARMAZENAGEM, RESERVA NACIONAL E PULMÃO REGIÃO→CIDADE (D-100, save v25).
// Ver docs/economy/storage_g2.md.
//
// O armazém é AGREGADO por (cidade, tipo, dono), nunca prédio a prédio:
//   * 6 tipos operacionais — silo, câmara fria, tanque de combustíveis,
//     armazenagem de gás, pátio de granéis e armazém geral;
//   * cofre e paiol militar existem só como ESTRUTURA nesta parte (sem fluxo
//     de estoque; a economia de guerra é uma parte futura);
//   * silo/tanque/gás/pátio ficam na REGIÃO; câmara fria/armazém geral na
//     CIDADE. A região é o pulmão: a colheita entra no silo regional e sai
//     para o mercado da cidade-polo aos poucos, pagando o custo de acesso
//     (estrada B5);
//   * donos separados: governo (reserva nacional, fora de venda) e privado
//     (produtor/cooperativa/comerciante, especulação real).
//
// O estoque de MERCADO continua em `City::markets` (não existe mercado
// nacional). `StorageSite::stock` é o estoque GUARDADO; compra/liberação move
// toneladas entre os dois SEM criar bens nem dinheiro: o comprador paga o
// vendedor por um caixa já existente (`government_budget` ↔ `firm_cash`) e a
// manutenção/acesso vai ao setor de serviços/transporte.
//
// O QUE É DADO: mapeamento produto→tipo e capacidade real (quando entrar na
// base, via `StorageService` e `set_storage_capacity`). O QUE É MODELO
// DECLARADO: meses de produção como capacidade inicial, custos, manutenção,
// cadência de liberação e heurísticas da IA (constantes em sim_constants.h).

#include "sim_types.h"

#include <string>

namespace ogs {

struct WorldState;

// Nome estável do tipo ("silo", "cold", "fuel_tank", "gas", "bulk_yard",
// "general", "vault", "magazine"). `nullptr` para tipo inválido.
const char* storage_type_name(int type);
// Inverso do nome (também aceita "camara_fria"/"armazem_geral" em pt-BR).
// false = nome desconhecido.
bool storage_type_from_name(const std::string& name, int& out);
// Nome do dono ("government"/"private"; aceita "state"/"governo"/"privado").
const char* storage_scope_name(int scope);
bool storage_scope_from_name(const std::string& name, int& out);
// O tipo fica na REGIÃO (pulmão) e não na cidade?
bool storage_type_is_region(int type);
// Tipo só de ESTRUTURA nesta parte (cofre/paiol): sem capacidade/estoque.
bool storage_type_is_structure_only(int type);
// Tipo guarda o produto? -1 se o produto não tem tipo.
bool storage_type_accepts(int type, int product_type);

// Reconstrói o mapeamento produto→tipo a partir dos nomes do catálogo
// (goods.json) e os índices de cidade dos armazéns. Overrides declarados por
// `set_storage_product_type` (dado da G2) sobrevivem enquanto o catálogo não
// mudar de tamanho. Idempotente.
void rebuild_storage_indexes(WorldState& ws);
// Declara/sobrescreve o tipo de um produto (carregador do JSON futuro).
void set_storage_product_type(WorldState& ws, int product, int type);

// Índice do armazém (cidade, tipo, dono) em `ws.storage_sites`; -1 se não
// existe. Um site só é criado quando há capacidade declarada/obra concluída.
int find_storage_site(const WorldState& ws, int city_index, int type,
	int scope);
// Garante o site (cria com capacidade 0 e estoque zerado). Índice em
// `ws.storage_sites`.
int ensure_storage_site(WorldState& ws, int city_index, int type, int scope);
// Soma capacidade ao site (cria se preciso) e garante o vetor de estoque no
// tamanho do catálogo.
void add_storage_capacity(WorldState& ws, int city_index, int type, int scope,
	double tons);
// Capacidade livre (t) considerando só os produtos que o tipo aceita.
double storage_free_capacity(const WorldState& ws, const StorageSite& site);
// Estoque guardado total (t) do site.
double storage_total_stock(const StorageSite& site);
// Estoque guardado de um produto (t); 0 fora do vetor.
double storage_stock(const StorageSite& site, int product);

// Aplica a obra concluída (§15) ao armazém: soma a capacidade ao dono. Chamado
// pelo `complete_project` (construction.cpp).
void apply_completed_storage_project(WorldState& ws,
	const ConstructionProject& project);

} // namespace ogs
