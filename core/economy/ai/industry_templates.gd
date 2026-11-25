extends RefCounted
class_name IndustryTemplates

## Parâmetros de engenharia das construções usados no world-gen (a mina
## inicial lê jobs/wage/opex daqui) e a classificação de recursos. A decisão
## da IA vive no C++ (`gdextension/src/sim/ai/`); os helpers GDScript foram
## removidos na limpeza D-131.

const TEMPLATES := {
	"wheat": {"output": 0, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"iron": {"output": 1, "input": -1, "jobs_per_ton": 3.35,
		"wage": 1.0, "opex_per_ton": 6.65, "input_ratio": 0.0},
	"rice": {"output": 2, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"maize": {"output": 3, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.0},
	"potato": {"output": 16, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 0.25},

	# --- Extração primária de minerais/energéticos (B4) ----------------------
	# Ordem de grandeza (não precisão): jobs_per_ton = empregos por t/dia,
	# wage = salário nominal por trabalhador/dia, opex_per_ton = custo variável
	# por tonelada. Base: mina de ferro do cenário demo (3,35 empregos/t, salário
	# 1,0, opex 6,65/t — a referência da cadeia viva). As demais derivam por
	# intensidade relativa: bulk a céu aberto (carvão, bauxita, fosfato, enxofre)
	# é mais barato e menos mão-de-obra/t; poços (petróleo/gás) são automatizados
	# (poucos empregos/t); metais profundos e metais de alto valor (cobre, chumbo,
	# tungstênio, terras raras, urânio) têm mais opex; metais preciosos (ouro,
	# diamante) têm alto opex/t mas preço muito superior. A profundidade da jazida
	# ainda multiplica o opex no núcleo (depth_opex_factor, até 3×).
	"coal": {"output": 12, "input": -1, "jobs_per_ton": 1.5,
		"wage": 1.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"copper": {"output": 13, "input": -1, "jobs_per_ton": 3.0,
		"wage": 1.0, "opex_per_ton": 8.0, "input_ratio": 0.0},
	"gas": {"output": 14, "input": -1, "jobs_per_ton": 0.6,
		"wage": 1.5, "opex_per_ton": 4.0, "input_ratio": 0.0},
	"oil": {"output": 15, "input": -1, "jobs_per_ton": 0.8,
		"wage": 1.5, "opex_per_ton": 6.0, "input_ratio": 0.0},
	"bauxite": {"output": 4, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 3.0, "input_ratio": 0.0},
	"lead": {"output": 5, "input": -1, "jobs_per_ton": 3.0,
		"wage": 1.0, "opex_per_ton": 8.0, "input_ratio": 0.0},
	"gold": {"output": 6, "input": -1, "jobs_per_ton": 8.0,
		"wage": 1.5, "opex_per_ton": 40.0, "input_ratio": 0.0},
	"phosphate": {"output": 7, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 3.0, "input_ratio": 0.0},
	"tungsten": {"output": 8, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.2, "opex_per_ton": 30.0, "input_ratio": 0.0},
	"rare_earths": {"output": 9, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.2, "opex_per_ton": 35.0, "input_ratio": 0.0},
	"sulfur": {"output": 10, "input": -1, "jobs_per_ton": 2.0,
		"wage": 1.0, "opex_per_ton": 2.0, "input_ratio": 0.0},
	"diamond": {"output": 11, "input": -1, "jobs_per_ton": 10.0,
		"wage": 1.5, "opex_per_ton": 50.0, "input_ratio": 0.0},
	"uranium": {"output": 20, "input": -1, "jobs_per_ton": 4.0,
		"wage": 1.5, "opex_per_ton": 45.0, "input_ratio": 0.0},

	# --- Renováveis não alimentares (B4) -------------------------------------
	# Soja/algodão são lavouras (mesma base do trigo: 7,5 empregos/t, salário 2,0,
	# opex 2,5/t); madeira é serraria/floresta (mais mecânica, menos gente). A
	# soja também é alimento da cesta (B3, fator calórico 1,3 — fonte USDA/FAO
	# em economy_sim.h): entra na cesta substituível, não mais como exportável.
	"soy": {"output": 17, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0, "caloric_factor": 1.3},
	"cotton": {"output": 18, "input": -1, "jobs_per_ton": 7.5,
		"wage": 2.0, "opex_per_ton": 2.5, "input_ratio": 0.0},
	"wood": {"output": 19, "input": -1, "jobs_per_ton": 5.0,
		"wage": 1.5, "opex_per_ton": 3.0, "input_ratio": 0.0},
}

## Cesta alimentar substituível (B3): trigo, arroz, milho, batata e soja.
## `caloric_factor` converte tonelada real em tonelada-equivalente de grão
## (batata ~1/4; soja ~1,3 — fonte FAO/USDA em economy_sim.h).
const FOOD_RESOURCES: Array[String] = ["wheat", "rice", "maize", "potato", "soy"]

## Minerais/energéticos (extração primária com jazida). A IA só constrói quando
## a cidade tem depósito com reserva restante (GeographyService declara via
## set_deposit).
const MINERAL_RESOURCES: Array[String] = [
	"iron", "coal", "copper", "gas", "oil", "bauxite", "lead", "gold",
	"phosphate", "tungsten", "rare_earths", "sulfur", "diamond", "uranium",
]

## Renováveis não alimentares exportáveis (aptidão agrícola/cobertura florestal).
## Soja saiu daqui: agora é alimento da cesta (B3).
const RENEWABLE_RESOURCES: Array[String] = ["cotton", "wood"]

## Bens cujo fator de produção divide a mesma terra/trabalho da cesta alimentar
## (lavouras industriais e silvicultura). A IA não os abre em cidade com déficit
## alimentar estrutural: a prioridade é comida, e a fazenda de algodão/madeira
## competiria pela bacia e pela mão de obra.
const LAND_COMPETING_RESOURCES: Array[String] = ["cotton", "wood"]

## Bens bulk de baixo valor (B4): extratores de alto volume e margem fina por
## tonelada, com reserva de décadas. O investidor privado usa um horizonte de
## payback maior para eles do que para metais/energéticos de margem alta — senão
## a mina de enxofre (margem ~7–11/t → payback 180–290 d) nunca fecha a conta de
## 150 dias e fica com extração 0. Só o enxofre entra aqui: carvão (margem ~2/t,
## payback ~1000 d) é fino demais até para o horizonte estendido e segue só com a
## estatal; fosfato/bauxita já fluem via `observed_cost` (a estatal abre e o
## investidor observa o custo real) — relaxá-los superexpandiria e tiraria
## firma/investimento dos renováveis (soja/madeira) que vêm depois na iteração.
const BULK_LOW_VALUE_RESOURCES: Array[String] = ["sulfur"]
