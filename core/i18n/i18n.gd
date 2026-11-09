extends Node

## Autoload `I18n`: tradução em tempo de execução para pt-BR, inglês e espanhol.
##
## As telas nunca escrevem texto fixo: pedem `I18n.t("chave")` e escutam
## `locale_changed` para se reconstruírem quando o idioma muda.

signal locale_changed(locale: String)

const DEFAULT_LOCALE := "pt_BR"
const SUPPORTED_LOCALES: Array[String] = ["pt_BR", "en", "es"]

const SHORT_LABELS := {"pt_BR": "PT", "en": "EN", "es": "ES"}

const STRINGS := {
	"pt_BR": {
		"common.close": "Fechar",
		"common.back": "Voltar",
		"common.na": "—",
		"common.per_day": "/dia",
		"month.1": "janeiro", "month.2": "fevereiro", "month.3": "março",
		"month.4": "abril", "month.5": "maio", "month.6": "junho",
		"month.7": "julho", "month.8": "agosto", "month.9": "setembro",
		"month.10": "outubro", "month.11": "novembro", "month.12": "dezembro",

		"hud.player_nation": "Sua nação",
		"hud.day": "Dia {day}",
		"hud.day_short": "Dia",
		"hud.paused": "Pausado",
		"hud.next_day": "Próximo dia",
		"hud.population": "População",
		"hud.stock": "Estoque",
		"hud.deficit": "Déficit",
		"hud.transit": "Em trânsito",
		"hud.speed": "Velocidade",
		"hud.language": "Idioma",

		"dock.economy": "Economia",
		"dock.trade": "Comércio",
		"dock.production": "Produção",
		"dock.politics": "Política",

		"econ.title": "Economia do trigo",
		"econ.tab.nations": "Nações",
		"econ.tab.cities": "Cidades",
		"econ.col.nation": "Nação",
		"econ.col.city": "Cidade",
		"econ.col.population": "População",
		"econ.col.production": "Produção/dia",
		"econ.col.need": "Demanda/dia",
		"econ.col.stock": "Estoque",
		"econ.col.deficit": "Déficit",
		"econ.col.transit": "Em trânsito",
		"econ.col.cities": "Cidades",
		"econ.summary": "{cities} cidades · {deficit} em déficit · {transit} em trânsito",
		"econ.hint": "Clique numa linha para abrir o detalhe da cidade.",
		"econ.all_fed": "Nenhuma cidade com déficit hoje.",

		"city.title": "Cidade",
		"city.subtitle": "{nation} · {population} hab.",
		"city.population": "População",
		"city.production": "Produção diária",
		"city.need": "Demanda diária",
		"city.consumed": "Consumo diário",
		"city.stock": "Estoque",
		"city.deficit": "Déficit diário",
		"city.dispatched": "Despachado hoje",
		"city.transit": "Em trânsito",
		"city.coverage": "Cobertura",
		"city.coverage_days": "{days} dias de estoque",
		"city.orders": "Pedidos desta cidade",
		"city.no_orders": "Nenhum pedido em aberto para esta cidade.",

		"trade.title": "Comércio e logística",
		"trade.tab.orders": "Pedidos",
		"trade.tab.routes": "Rotas",
		"trade.col.from": "Origem",
		"trade.col.to": "Destino",
		"trade.col.scope": "Mercado",
		"trade.col.amount": "Carga",
		"trade.col.hours": "Trânsito",
		"trade.col.freight": "Frete",
		"trade.col.mode": "Modo",
		"trade.col.distance": "Distância",
		"trade.col.trips": "Viagens",
		"trade.empty": "Nenhum pedido de comércio neste dia.",
		"trade.select": "Selecione um pedido para ver os trechos.",
		"trade.today": "Pedidos do dia",
		"trade.history": "Histórico recente",
		"trade.legs": "Trechos do pedido",

		"prod.title": "Produção e construção",
		"prod.tab.queue": "Fila de obras",
		"prod.tab.facilities": "Instalações",
		"prod.empty.title": "Nenhuma construção planejada",
		"prod.empty.body": "A simulação ainda não cobra custo de construção nem mantém fila de produção. Quando isso existir, as obras aparecem aqui.",
		"prod.facilities.body": "A economia atual modela apenas a produção diária de trigo de cada cidade, sem prédios ou oficinas individuais.",

		"pol.title": "Política e diplomacia",
		"pol.tab.nations": "Nações",
		"pol.tab.diplomacy": "Diplomacia",
		"pol.tab.dossier": "Dossiê",
		"pol.col.nation": "Nação",
		"pol.col.cities": "Cidades",
		"pol.col.population": "População",
		"pol.col.status": "Situação",
		"pol.status.peace": "Em paz",
		"pol.relations.empty": "Relações diplomáticas ainda não são simuladas. Todas as nações estão em paz.",
		"pol.dossier.empty": "O dossiê resume cada nação pelo que a simulação já conhece: população, cidades e situação alimentar.",
		"pol.dossier.line": "{nation}: {cities} cidade(s), {population} habitantes, déficit de {deficit} t/dia.",
		"pol.dossier.healthy": "{nation}: {cities} cidade(s), {population} habitantes, sem déficit.",
		"pol.your_nation": "sua nação",

		"scope.regional": "Regional",
		"scope.nacional": "Nacional",
		"scope.internacional": "Internacional",

		"mode.truck": "Caminhão",
		"mode.ship": "Navio",
		"mode.plane": "Avião",

		"nation.GBR": "Inglaterra",
		"nation.FRA": "França",
		"nation.DEU": "Alemanha",
		"nation.NED": "Holanda",
		"nation.BEL": "Bélgica",

		"city.name.GBR-LDN": "Londres",
		"city.name.FRA-PAR": "Paris",
		"city.name.DEU-BER": "Berlim",
		"city.name.NED-AMS": "Amsterdã",
		"city.name.BEL-BRU": "Bruxelas",
	},
	"en": {
		"common.close": "Close",
		"common.back": "Back",
		"common.na": "—",
		"common.per_day": "/day",
		"month.1": "January", "month.2": "February", "month.3": "March",
		"month.4": "April", "month.5": "May", "month.6": "June",
		"month.7": "July", "month.8": "August", "month.9": "September",
		"month.10": "October", "month.11": "November", "month.12": "December",

		"hud.player_nation": "Your nation",
		"hud.day": "Day {day}",
		"hud.day_short": "Day",
		"hud.paused": "Paused",
		"hud.next_day": "Next day",
		"hud.population": "Population",
		"hud.stock": "Stockpile",
		"hud.deficit": "Deficit",
		"hud.transit": "In transit",
		"hud.speed": "Speed",
		"hud.language": "Language",

		"dock.economy": "Economy",
		"dock.trade": "Trade",
		"dock.production": "Production",
		"dock.politics": "Politics",

		"econ.title": "Wheat economy",
		"econ.tab.nations": "Nations",
		"econ.tab.cities": "Cities",
		"econ.col.nation": "Nation",
		"econ.col.city": "City",
		"econ.col.population": "Population",
		"econ.col.production": "Production/day",
		"econ.col.need": "Demand/day",
		"econ.col.stock": "Stockpile",
		"econ.col.deficit": "Deficit",
		"econ.col.transit": "In transit",
		"econ.col.cities": "Cities",
		"econ.summary": "{cities} cities · {deficit} in deficit · {transit} in transit",
		"econ.hint": "Click a row to open the city detail.",
		"econ.all_fed": "No city is in deficit today.",

		"city.title": "City",
		"city.subtitle": "{nation} · {population} people",
		"city.population": "Population",
		"city.production": "Daily production",
		"city.need": "Daily demand",
		"city.consumed": "Daily consumption",
		"city.stock": "Stockpile",
		"city.deficit": "Daily deficit",
		"city.dispatched": "Dispatched today",
		"city.transit": "In transit",
		"city.coverage": "Coverage",
		"city.coverage_days": "{days} days of stock",
		"city.orders": "This city's orders",
		"city.no_orders": "No open order for this city.",

		"trade.title": "Trade and logistics",
		"trade.tab.orders": "Orders",
		"trade.tab.routes": "Routes",
		"trade.col.from": "From",
		"trade.col.to": "To",
		"trade.col.scope": "Market",
		"trade.col.amount": "Load",
		"trade.col.hours": "Transit",
		"trade.col.freight": "Freight",
		"trade.col.mode": "Mode",
		"trade.col.distance": "Distance",
		"trade.col.trips": "Trips",
		"trade.empty": "No trade order today.",
		"trade.select": "Select an order to see its legs.",
		"trade.today": "Today's orders",
		"trade.history": "Recent orders",
		"trade.legs": "Order legs",

		"prod.title": "Production and construction",
		"prod.tab.queue": "Build queue",
		"prod.tab.facilities": "Facilities",
		"prod.empty.title": "Nothing under construction",
		"prod.empty.body": "The simulation has no build cost or production queue yet. Once it does, works appear here.",
		"prod.facilities.body": "The current economy only models each city's daily wheat output, with no individual buildings or workshops.",

		"pol.title": "Politics and diplomacy",
		"pol.tab.nations": "Nations",
		"pol.tab.diplomacy": "Diplomacy",
		"pol.tab.dossier": "Dossier",
		"pol.col.nation": "Nation",
		"pol.col.cities": "Cities",
		"pol.col.population": "Population",
		"pol.col.status": "Status",
		"pol.status.peace": "At peace",
		"pol.relations.empty": "Diplomatic relations are not simulated yet. All nations are at peace.",
		"pol.dossier.empty": "The dossier sums up each nation using what the simulation knows: population, cities and food situation.",
		"pol.dossier.line": "{nation}: {cities} city(ies), {population} people, deficit of {deficit} t/day.",
		"pol.dossier.healthy": "{nation}: {cities} city(ies), {population} people, no deficit.",
		"pol.your_nation": "your nation",

		"scope.regional": "Regional",
		"scope.nacional": "National",
		"scope.internacional": "International",

		"mode.truck": "Truck",
		"mode.ship": "Ship",
		"mode.plane": "Plane",

		"nation.GBR": "England",
		"nation.FRA": "France",
		"nation.DEU": "Germany",
		"nation.NED": "Netherlands",
		"nation.BEL": "Belgium",

		"city.name.GBR-LDN": "London",
		"city.name.FRA-PAR": "Paris",
		"city.name.DEU-BER": "Berlin",
		"city.name.NED-AMS": "Amsterdam",
		"city.name.BEL-BRU": "Brussels",
	},
	"es": {
		"common.close": "Cerrar",
		"common.back": "Volver",
		"common.na": "—",
		"common.per_day": "/día",
		"month.1": "enero", "month.2": "febrero", "month.3": "marzo",
		"month.4": "abril", "month.5": "mayo", "month.6": "junio",
		"month.7": "julio", "month.8": "agosto", "month.9": "septiembre",
		"month.10": "octubre", "month.11": "noviembre", "month.12": "diciembre",

		"hud.player_nation": "Tu nación",
		"hud.day": "Día {day}",
		"hud.day_short": "Día",
		"hud.paused": "Pausado",
		"hud.next_day": "Próximo día",
		"hud.population": "Población",
		"hud.stock": "Reserva",
		"hud.deficit": "Déficit",
		"hud.transit": "En tránsito",
		"hud.speed": "Velocidad",
		"hud.language": "Idioma",

		"dock.economy": "Economía",
		"dock.trade": "Comercio",
		"dock.production": "Producción",
		"dock.politics": "Política",

		"econ.title": "Economía del trigo",
		"econ.tab.nations": "Naciones",
		"econ.tab.cities": "Ciudades",
		"econ.col.nation": "Nación",
		"econ.col.city": "Ciudad",
		"econ.col.population": "Población",
		"econ.col.production": "Producción/día",
		"econ.col.need": "Demanda/día",
		"econ.col.stock": "Reserva",
		"econ.col.deficit": "Déficit",
		"econ.col.transit": "En tránsito",
		"econ.col.cities": "Ciudades",
		"econ.summary": "{cities} ciudades · {deficit} con déficit · {transit} en tránsito",
		"econ.hint": "Haz clic en una fila para abrir el detalle de la ciudad.",
		"econ.all_fed": "Ninguna ciudad tiene déficit hoy.",

		"city.title": "Ciudad",
		"city.subtitle": "{nation} · {population} hab.",
		"city.population": "Población",
		"city.production": "Producción diaria",
		"city.need": "Demanda diaria",
		"city.consumed": "Consumo diario",
		"city.stock": "Reserva",
		"city.deficit": "Déficit diario",
		"city.dispatched": "Despachado hoy",
		"city.transit": "En tránsito",
		"city.coverage": "Cobertura",
		"city.coverage_days": "{days} días de reserva",
		"city.orders": "Pedidos de esta ciudad",
		"city.no_orders": "Ningún pedido abierto para esta ciudad.",

		"trade.title": "Comercio y logística",
		"trade.tab.orders": "Pedidos",
		"trade.tab.routes": "Rutas",
		"trade.col.from": "Origen",
		"trade.col.to": "Destino",
		"trade.col.scope": "Mercado",
		"trade.col.amount": "Carga",
		"trade.col.hours": "Tránsito",
		"trade.col.freight": "Flete",
		"trade.col.mode": "Modo",
		"trade.col.distance": "Distancia",
		"trade.col.trips": "Viajes",
		"trade.empty": "Ningún pedido de comercio hoy.",
		"trade.select": "Selecciona un pedido para ver sus tramos.",
		"trade.today": "Pedidos del día",
		"trade.history": "Historial reciente",
		"trade.legs": "Tramos del pedido",

		"prod.title": "Producción y construcción",
		"prod.tab.queue": "Cola de obras",
		"prod.tab.facilities": "Instalaciones",
		"prod.empty.title": "Ninguna construcción planeada",
		"prod.empty.body": "La simulación todavía no cobra coste de construcción ni mantiene cola de producción. Cuando exista, las obras aparecerán aquí.",
		"prod.facilities.body": "La economía actual solo modela la producción diaria de trigo de cada ciudad, sin edificios ni talleres individuales.",

		"pol.title": "Política y diplomacia",
		"pol.tab.nations": "Naciones",
		"pol.tab.diplomacy": "Diplomacia",
		"pol.tab.dossier": "Dosier",
		"pol.col.nation": "Nación",
		"pol.col.cities": "Ciudades",
		"pol.col.population": "Población",
		"pol.col.status": "Situación",
		"pol.status.peace": "En paz",
		"pol.relations.empty": "Las relaciones diplomáticas aún no se simulan. Todas las naciones están en paz.",
		"pol.dossier.empty": "El dosier resume cada nación con lo que la simulación ya conoce: población, ciudades y situación alimentaria.",
		"pol.dossier.line": "{nation}: {cities} ciudad(es), {population} habitantes, déficit de {deficit} t/día.",
		"pol.dossier.healthy": "{nation}: {cities} ciudad(es), {population} habitantes, sin déficit.",
		"pol.your_nation": "tu nación",

		"scope.regional": "Regional",
		"scope.nacional": "Nacional",
		"scope.internacional": "Internacional",

		"mode.truck": "Camión",
		"mode.ship": "Barco",
		"mode.plane": "Avión",

		"nation.GBR": "Inglaterra",
		"nation.FRA": "Francia",
		"nation.DEU": "Alemania",
		"nation.NED": "Países Bajos",
		"nation.BEL": "Bélgica",

		"city.name.GBR-LDN": "Londres",
		"city.name.FRA-PAR": "París",
		"city.name.DEU-BER": "Berlín",
		"city.name.NED-AMS": "Ámsterdam",
		"city.name.BEL-BRU": "Bruselas",
	},
}

var locale: String = DEFAULT_LOCALE


## Traduz uma chave; aceita parâmetros no formato `{nome}`.
func t(key: String, params: Dictionary = {}) -> String:
	var table: Dictionary = STRINGS.get(locale, {})
	var fallback: Dictionary = STRINGS[DEFAULT_LOCALE]
	var text: String = table.get(key, fallback.get(key, key))
	if params.is_empty():
		return text
	return text.format(params)


## Troca o idioma ativo e avisa as telas.
func set_locale(code: String) -> void:
	if not SUPPORTED_LOCALES.has(code) or code == locale:
		return
	locale = code
	locale_changed.emit(locale)


## Próximo idioma do ciclo PT → EN → ES → PT.
func next_locale() -> String:
	var index: int = SUPPORTED_LOCALES.find(locale)
	return SUPPORTED_LOCALES[(index + 1) % SUPPORTED_LOCALES.size()]


func short_label() -> String:
	return SHORT_LABELS.get(locale, "PT")


## Nome localizado de uma nação a partir da tag (GBR, FRA...).
func nation_name(tag: String) -> String:
	return t("nation.%s" % tag)


## Nome localizado de uma cidade a partir do id (GBR-LDN...).
func city_name(city_id: String, fallback: String) -> String:
	var key := "city.name.%s" % city_id
	var value := t(key)
	return fallback if value == key else value
