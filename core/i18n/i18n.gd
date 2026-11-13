extends Node

## Autoload `I18n`: tradução em tempo de execução para pt-BR, inglês e espanhol.
##
## As telas nunca escrevem texto fixo: pedem `I18n.t("chave")` e escutam
## `locale_changed` para se reconstruírem quando o idioma muda.

signal locale_changed(locale: String)

const DEFAULT_LOCALE := "pt_BR"
const SUPPORTED_LOCALES: Array[String] = ["pt_BR", "en", "es"]

const SHORT_LABELS := {"pt_BR": "PT", "en": "EN", "es": "ES"}
const I18N_PATH := "res://data/definitions/i18n/i18n.json"
const SETTINGS_PATH := "user://settings.cfg"

## Textos do gerador de telas (paleta C) e dos cinco templates.
## Ficam aqui, e não em data/definitions, porque esta etapa não altera a
## fonte de dados congelada; são mesclados sobre o JSON ao carregar.
const TEMPLATE_STRINGS := {
	"pt_BR": {
		"tpl.common.page": "Página {page} de {pages}",
		"tpl.common.result": "{n} resultados",
		"tpl.footer.panels": "1–5 painéis",
		"tpl.footer.tabs": "Tab navegar",
		"tpl.footer.close": "fechar",
		"tpl.footer.table": "Clique numa linha para abrir a ficha · ↑↓ navegar",
		"tpl.footer.search": "/ buscar",
		"tpl.footer.filters": "F filtros",
		"tpl.footer.indicator": "I trocar indicador",
		"tpl.footer.group": "G trocar grupo",
		"tpl.footer.tabs_switch": "← → trocar aba · ESC fechar",

		"tpl.dossie.crumb": "Política › Dossiê",
		"tpl.dossie.title": "Dossiê da nação",
		"tpl.dossie.seal": "CONFIDENCIAL · Nº {number}",
		"tpl.dossie.capital_line": "Capital · {city} · {government}",
		"tpl.dossie.gov.monarchy": "Monarquia parlamentar",
		"tpl.dossie.cities": "Cidades",
		"tpl.dossie.provinces": "Províncias",
		"tpl.dossie.ports": "Portos",
		"tpl.dossie.area": "Território",
		"tpl.dossie.own": "SUA NAÇÃO",
		"tpl.dossie.open_economy": "Abrir economia",
		"tpl.dossie.view_map": "Ver no mapa",
		"tpl.dossie.kpi.population": "População",
		"tpl.dossie.kpi.gdp": "PIB (PPC)",
		"tpl.dossie.kpi.food": "Comida",
		"tpl.dossie.kpi.cash": "Caixa do governo",
		"tpl.dossie.delta.population": "▲ 0,3% ao ano",
		"tpl.dossie.delta.gdp": "▲ 1,1%",
		"tpl.dossie.delta.food": "cobertura de 5 dias",
		"tpl.dossie.delta.cash": "▼ 2,4% no mês",
		"tpl.dossie.resources": "Recursos estratégicos",
		"tpl.dossie.relations": "Relações",
		"tpl.dossie.resources.note": "Autossuficiência = produção ÷ consumo dos últimos 30 dias.",
		"tpl.dossie.relations.note": "Diplomacia ainda não simulada — status fixo do cenário 2026.",
		"tpl.relation.ally": "Aliado",
		"tpl.relation.neutral": "Neutro",
		"tpl.relation.hostile": "Hostil",

		"tpl.registro.crumb": "Economia ›",
		"tpl.registro.title": "Registro de cidades",
		"tpl.registro.search": "Buscar cidade…",
		"tpl.registro.filter.all": "Todas · {n}",
		"tpl.registro.filter.deficit": "Com déficit · {n}",
		"tpl.registro.filter.ports": "Portos · {n}",
		"tpl.registro.filter.capitals": "Capitais · {n}",
		"tpl.registro.col.city": "Cidade",
		"tpl.registro.col.population": "População",
		"tpl.registro.col.food": "Comida",
		"tpl.registro.col.stock": "Estoque",
		"tpl.registro.col.deficit": "Déficit",
		"tpl.registro.col.deposits": "Jazidas",
		"tpl.registro.selected": "Ficha selecionada",
		"tpl.registro.tag.capital": "Capital",
		"tpl.registro.tag.deficit": "Déficit",
		"tpl.registro.tag.port": "Porto: {port}",
		"tpl.registro.province": "Província",
		"tpl.registro.area": "Área administrada",
		"tpl.registro.employed": "Empregados",
		"tpl.registro.wage": "Salário médio",
		"tpl.registro.note": "Trigo abaixo do consumo há 6 dias. Importações de Rotterdam chegam em 2 dias.",
		"tpl.registro.open_sheet": "Abrir ficha completa",
		"tpl.registro.title.nations": "Registro de nações",
		"tpl.registro.search.nation": "Buscar nação…",
		"tpl.registro.filter.transit": "Com trânsito · {n}",
		"tpl.registro.footer.select": "Clique para selecionar · Enter ou duplo clique abre a ficha",
		"tpl.registro.hint.nations": "Duplo clique numa nação abre a primeira cidade.",
		"tpl.registro.note.coverage": "Estoque cobre {days} dias de demanda.",
		"tpl.registro.note.short": "Demanda não atendida hoje: {amount}; estoque cobre {days} dias.",
		"tpl.registro.note.empty": "Sem demanda registrada neste dia.",
		"tpl.registro.empty.nations": "A simulação ainda não publicou nações.",
		"tpl.registro.empty.nations_filtered": "Nenhuma nação corresponde à busca.",
		"tpl.registro.empty.nations_body": "Ajuste a busca para ver a lista completa.",
		"tpl.registro.empty.cities": "A simulação ainda não publicou cidades.",
		"tpl.registro.empty.wait": "Os números aparecem quando o primeiro dia for publicado.",
		"tpl.registro.demo.nation": "Reino Unido",
		"tpl.registro.demo.note": "Estoque cobre 3,1 dias de demanda.",

		"tpl.ficha.tab.overview": "Visão geral",
		"tpl.ficha.tab.population": "População",
		"tpl.ficha.tab.production": "Produção",
		"tpl.ficha.tab.market": "Mercado",
		"tpl.ficha.tab.resources": "Recursos",
		"tpl.ficha.crumb": "Reino Unido › Inglaterra › Greater London",
		"tpl.ficha.day": "DIA {day}",
		"tpl.ficha.tag.capital": "Capital",
		"tpl.ficha.tag.port": "Porto: Tilbury",
		"tpl.ficha.tag.provinces": "18 províncias",
		"tpl.ficha.stamp": "DÉFICIT DE TRIGO",
		"tpl.ficha.kpi.population": "População",
		"tpl.ficha.kpi.employed": "Empregados",
		"tpl.ficha.kpi.gdp": "PIB local",
		"tpl.ficha.supply": "Abastecimento · últimos 30 dias",
		"tpl.ficha.supply.note": "Cobertura de comida (%) — vermelho abaixo de 70%",
		"tpl.ficha.date.start": "4 dez",
		"tpl.ficha.date.end": "3 jan",
		"tpl.ficha.produces": "Produz",
		"tpl.ficha.consumes": "Consome",
		"tpl.ficha.industrial_goods": "Bens industriais",
		"tpl.ficha.services": "Serviços",
		"tpl.ficha.import_wheat": "Importar trigo",
		"tpl.ficha.center": "Centralizar no mapa",
		"tpl.ficha.pending.title": "Ainda não simulado",
		"tpl.ficha.pending.body": "Esta aba depende de dados que a simulação atual não publica. Nada é inventado aqui.",
		"tpl.ficha.preview.title": "Prévia da aba",
		"tpl.ficha.preview.body": "Esta prévia cobre a aba Visão geral. O conteúdo desta aba é definido pela spec que usar o gerador.",

		"tpl.telegram.title": "TELEGRAMA",
		"tpl.telegram.meta": "Nº {number} · {status}",
		"tpl.telegram.urgent": "URGENTE",
		"tpl.telegram.from": "DE:",
		"tpl.telegram.to": "PARA:",
		"tpl.telegram.subject": "ASSUNTO:",
		"tpl.telegram.from.value": "Ministério do Abastecimento",
		"tpl.telegram.to.value": "Gabinete do Primeiro-Ministro",
		"tpl.telegram.subject.value": "Escassez de trigo em Londres",
		"tpl.telegram.body": "ESTOQUES DE TRIGO EM LONDRES CAEM HÁ SEIS DIAS PT COBERTURA EM 66 POR CENTO PT PORTO DE TILBURY CONGESTIONADO PT AGUARDAMOS INSTRUÇÕES PT",
		"tpl.telegram.choice_label": "Escolha uma resposta",
		"tpl.telegram.choice.import": "A · Importar trigo de emergência",
		"tpl.telegram.choice.import.note": "−$ 1,2 bi do caixa · cobertura volta a 85% em 3 dias",
		"tpl.telegram.choice.ration": "B · Racionar consumo",
		"tpl.telegram.choice.ration.note": "Consumo −15% por 30 dias · satisfação −8",
		"tpl.telegram.choice.wait": "C · Aguardar o mercado",
		"tpl.telegram.choice.wait.note": "Sem custo · risco de fome se o déficit continuar",
		"tpl.telegram.footer": "Recebido {date} · o jogo fica pausado até a decisão",
		"tpl.telegram.received": "3 jan 2026 · 01:00",
		"tpl.telegram.stamp": "PRIORIDADE",

		"tpl.quadro.crumb": "Estatísticas ›",
		"tpl.quadro.title": "Quadro comparativo",
		"tpl.quadro.indicator": "Indicador",
		"tpl.quadro.ind.population": "População",
		"tpl.quadro.ind.gdp": "PIB",
		"tpl.quadro.ind.food": "Comida",
		"tpl.quadro.ind.minerals": "Reservas minerais",
		"tpl.quadro.group": "Grupo",
		"tpl.quadro.group.g7": "G7",
		"tpl.quadro.group.g7_select": "G7 ▾",
		"tpl.quadro.ranking": "Ranking · População (milhões)",
		"tpl.quadro.position": "Sua posição",
		"tpl.quadro.position.note": "de {group} no {group_name} · {world}º de {world_total} no mundo",
		"tpl.quadro.rank_suffix": "º",
		"tpl.quadro.above": "Acima",
		"tpl.quadro.below": "Abaixo",
		"tpl.quadro.above.value": "Alemanha · +16 M",
		"tpl.quadro.below.value": "França · −2 M",
		"tpl.quadro.trend": "Tendência · 12 meses",
		"tpl.quadro.source": "Fonte: cenário 2026 (ONU WPP). Sua nação em vermelho.",
		"tpl.quadro.month.start": "jan 2025",
		"tpl.quadro.month.end": "jan 2026",

		"tpl.menu.title": "Open Grand Strategy",
		"tpl.menu.label": "ARQUIVO DO GABINETE",
		"tpl.menu.label_meta": "DIA {day}",
		"tpl.menu.paused": "— JOGO PAUSADO —",
		"tpl.menu.stamp": "PAUSADO",
		"tpl.menu.save": "Salvar jogo",
		"tpl.menu.load": "Carregar jogo",
		"tpl.menu.soon": "Em breve: o jogo ainda não tem salvamento.",
		"tpl.menu.hint.settings": "idioma · tela",
		"tpl.menu.hint.shortcuts": "teclado",
		"tpl.menu.shortcuts": "Atalhos",
		"tpl.menu.quit": "Sair para a área de trabalho",
		"tpl.menu.last_save": "Último salvamento: {when}",
		"tpl.menu.version": "pré-alpha",
		"tpl.menu.back": "Voltar",
		"tpl.menu.sheet.settings": "CONFIGURAÇÕES",
		"tpl.menu.sheet.shortcuts": "ATALHOS",
		"tpl.menu.sheet.credits": "CRÉDITOS",
		"tpl.menu.active": "ATIVO",
		"tpl.menu.fullscreen": "Tela cheia",
		"tpl.menu.fullscreen.on": "ligada",
		"tpl.menu.fullscreen.off": "desligada",
		"tpl.menu.key.esc": "ESC",
		"tpl.menu.key.panels": "1–5",
		"tpl.menu.key.fullscreen": "F11",
		"tpl.menu.key.focus": "TAB",
		"tpl.menu.shortcut.esc": "Fechar sobreposições e abrir o menu",
		"tpl.menu.shortcut.panels": "Abrir os painéis 1 a 5",
		"tpl.menu.shortcut.fullscreen": "Alternar tela cheia",
		"tpl.menu.shortcut.focus": "Navegar com Tab e setas; Enter ativa",
		"hud.credits.body": "Open Grand Strategy — simulação econômica de código aberto. Imagens de satélite © Esri, Maxar, Earthstar Geographics.",
	},
	"en": {
		"tpl.common.page": "Page {page} of {pages}",
		"tpl.common.result": "{n} results",
		"tpl.footer.panels": "1–5 panels",
		"tpl.footer.tabs": "Tab to navigate",
		"tpl.footer.close": "close",
		"tpl.footer.table": "Click a row to open the sheet · ↑↓ navigate",
		"tpl.footer.search": "/ search",
		"tpl.footer.filters": "F filters",
		"tpl.footer.indicator": "I switch indicator",
		"tpl.footer.group": "G switch group",
		"tpl.footer.tabs_switch": "← → switch tab · ESC close",

		"tpl.dossie.crumb": "Politics › Dossier",
		"tpl.dossie.title": "Nation dossier",
		"tpl.dossie.seal": "CONFIDENTIAL · No. {number}",
		"tpl.dossie.capital_line": "Capital · {city} · {government}",
		"tpl.dossie.gov.monarchy": "Parliamentary monarchy",
		"tpl.dossie.cities": "Cities",
		"tpl.dossie.provinces": "Provinces",
		"tpl.dossie.ports": "Ports",
		"tpl.dossie.area": "Territory",
		"tpl.dossie.own": "YOUR NATION",
		"tpl.dossie.open_economy": "Open economy",
		"tpl.dossie.view_map": "Show on map",
		"tpl.dossie.kpi.population": "Population",
		"tpl.dossie.kpi.gdp": "GDP (PPP)",
		"tpl.dossie.kpi.food": "Food",
		"tpl.dossie.kpi.cash": "Government treasury",
		"tpl.dossie.delta.population": "▲ 0.3% per year",
		"tpl.dossie.delta.gdp": "▲ 1.1%",
		"tpl.dossie.delta.food": "5 days of coverage",
		"tpl.dossie.delta.cash": "▼ 2.4% this month",
		"tpl.dossie.resources": "Strategic resources",
		"tpl.dossie.relations": "Relations",
		"tpl.dossie.resources.note": "Self-sufficiency = production ÷ consumption over the last 30 days.",
		"tpl.dossie.relations.note": "Diplomacy is not simulated yet — fixed status for the 2026 scenario.",
		"tpl.relation.ally": "Ally",
		"tpl.relation.neutral": "Neutral",
		"tpl.relation.hostile": "Hostile",

		"tpl.registro.crumb": "Economy ›",
		"tpl.registro.title": "City register",
		"tpl.registro.search": "Search city…",
		"tpl.registro.filter.all": "All · {n}",
		"tpl.registro.filter.deficit": "In deficit · {n}",
		"tpl.registro.filter.ports": "Ports · {n}",
		"tpl.registro.filter.capitals": "Capitals · {n}",
		"tpl.registro.col.city": "City",
		"tpl.registro.col.population": "Population",
		"tpl.registro.col.food": "Food",
		"tpl.registro.col.stock": "Stock",
		"tpl.registro.col.deficit": "Deficit",
		"tpl.registro.col.deposits": "Deposits",
		"tpl.registro.selected": "Selected sheet",
		"tpl.registro.tag.capital": "Capital",
		"tpl.registro.tag.deficit": "Deficit",
		"tpl.registro.tag.port": "Port: {port}",
		"tpl.registro.province": "Province",
		"tpl.registro.area": "Administered area",
		"tpl.registro.employed": "Employed",
		"tpl.registro.wage": "Average wage",
		"tpl.registro.note": "Wheat has been below consumption for 6 days. Imports from Rotterdam arrive in 2 days.",
		"tpl.registro.open_sheet": "Open full sheet",
		"tpl.registro.title.nations": "Nation register",
		"tpl.registro.search.nation": "Search nation…",
		"tpl.registro.filter.transit": "In transit · {n}",
		"tpl.registro.footer.select": "Click to select · Enter or double click opens the sheet",
		"tpl.registro.hint.nations": "Double click a nation to open its first city.",
		"tpl.registro.note.coverage": "Stock covers {days} days of demand.",
		"tpl.registro.note.short": "Unmet demand today: {amount}; stock covers {days} days.",
		"tpl.registro.note.empty": "No demand recorded this day.",
		"tpl.registro.empty.nations": "The simulation has not published nations yet.",
		"tpl.registro.empty.nations_filtered": "No nation matches the search.",
		"tpl.registro.empty.nations_body": "Adjust the search to see the full list.",
		"tpl.registro.empty.cities": "The simulation has not published cities yet.",
		"tpl.registro.empty.wait": "Numbers appear once the first day is published.",
		"tpl.registro.demo.nation": "United Kingdom",
		"tpl.registro.demo.note": "Stock covers 3.1 days of demand.",

		"tpl.ficha.tab.overview": "Overview",
		"tpl.ficha.tab.population": "Population",
		"tpl.ficha.tab.production": "Production",
		"tpl.ficha.tab.market": "Market",
		"tpl.ficha.tab.resources": "Resources",
		"tpl.ficha.crumb": "United Kingdom › England › Greater London",
		"tpl.ficha.day": "DAY {day}",
		"tpl.ficha.tag.capital": "Capital",
		"tpl.ficha.tag.port": "Port: Tilbury",
		"tpl.ficha.tag.provinces": "18 provinces",
		"tpl.ficha.stamp": "WHEAT DEFICIT",
		"tpl.ficha.kpi.population": "Population",
		"tpl.ficha.kpi.employed": "Employed",
		"tpl.ficha.kpi.gdp": "Local GDP",
		"tpl.ficha.supply": "Supply · last 30 days",
		"tpl.ficha.supply.note": "Food coverage (%) — red below 70%",
		"tpl.ficha.date.start": "Dec 4",
		"tpl.ficha.date.end": "Jan 3",
		"tpl.ficha.produces": "Produces",
		"tpl.ficha.consumes": "Consumes",
		"tpl.ficha.industrial_goods": "Industrial goods",
		"tpl.ficha.services": "Services",
		"tpl.ficha.import_wheat": "Import wheat",
		"tpl.ficha.center": "Center on map",
		"tpl.ficha.pending.title": "Not simulated yet",
		"tpl.ficha.pending.body": "This tab depends on data the current simulation does not publish. Nothing is invented here.",
		"tpl.ficha.preview.title": "Tab preview",
		"tpl.ficha.preview.body": "This preview covers the Overview tab. The content of this tab is defined by whichever spec uses the builder.",

		"tpl.telegram.title": "TELEGRAM",
		"tpl.telegram.meta": "No. {number} · {status}",
		"tpl.telegram.urgent": "URGENT",
		"tpl.telegram.from": "FROM:",
		"tpl.telegram.to": "TO:",
		"tpl.telegram.subject": "SUBJECT:",
		"tpl.telegram.from.value": "Ministry of Supply",
		"tpl.telegram.to.value": "Office of the Prime Minister",
		"tpl.telegram.subject.value": "Wheat shortage in London",
		"tpl.telegram.body": "WHEAT STOCKS IN LONDON HAVE FALLEN FOR SIX DAYS STOP COVERAGE AT 66 PER CENT STOP PORT OF TILBURY CONGESTED STOP AWAITING INSTRUCTIONS STOP",
		"tpl.telegram.choice_label": "Choose a reply",
		"tpl.telegram.choice.import": "A · Import emergency wheat",
		"tpl.telegram.choice.import.note": "−$1.2 bn from the treasury · coverage returns to 85% in 3 days",
		"tpl.telegram.choice.ration": "B · Ration consumption",
		"tpl.telegram.choice.ration.note": "Consumption −15% for 30 days · satisfaction −8",
		"tpl.telegram.choice.wait": "C · Wait for the market",
		"tpl.telegram.choice.wait.note": "No cost · risk of famine if the deficit continues",
		"tpl.telegram.footer": "Received {date} · the game stays paused until the decision",
		"tpl.telegram.received": "3 Jan 2026 · 01:00",
		"tpl.telegram.stamp": "PRIORITY",

		"tpl.quadro.crumb": "Statistics ›",
		"tpl.quadro.title": "Comparison board",
		"tpl.quadro.indicator": "Indicator",
		"tpl.quadro.ind.population": "Population",
		"tpl.quadro.ind.gdp": "GDP",
		"tpl.quadro.ind.food": "Food",
		"tpl.quadro.ind.minerals": "Mineral reserves",
		"tpl.quadro.group": "Group",
		"tpl.quadro.group.g7": "G7",
		"tpl.quadro.group.g7_select": "G7 ▾",
		"tpl.quadro.ranking": "Ranking · Population (millions)",
		"tpl.quadro.position": "Your position",
		"tpl.quadro.position.note": "of {group} in the {group_name} · {world}th of {world_total} worldwide",
		"tpl.quadro.rank_suffix": "th",
		"tpl.quadro.above": "Above",
		"tpl.quadro.below": "Below",
		"tpl.quadro.above.value": "Germany · +16 M",
		"tpl.quadro.below.value": "France · −2 M",
		"tpl.quadro.trend": "Trend · 12 months",
		"tpl.quadro.source": "Source: 2026 scenario (UN WPP). Your nation in red.",
		"tpl.quadro.month.start": "Jan 2025",
		"tpl.quadro.month.end": "Jan 2026",

		"tpl.menu.title": "Open Grand Strategy",
		"tpl.menu.label": "CABINET FILE",
		"tpl.menu.label_meta": "DAY {day}",
		"tpl.menu.paused": "— GAME PAUSED —",
		"tpl.menu.stamp": "PAUSED",
		"tpl.menu.save": "Save game",
		"tpl.menu.load": "Load game",
		"tpl.menu.soon": "Coming soon: the game has no save system yet.",
		"tpl.menu.hint.settings": "language · screen",
		"tpl.menu.hint.shortcuts": "keyboard",
		"tpl.menu.shortcuts": "Shortcuts",
		"tpl.menu.quit": "Quit to desktop",
		"tpl.menu.last_save": "Last save: {when}",
		"tpl.menu.version": "pre-alpha",
		"tpl.menu.back": "Back",
		"tpl.menu.sheet.settings": "SETTINGS",
		"tpl.menu.sheet.shortcuts": "SHORTCUTS",
		"tpl.menu.sheet.credits": "CREDITS",
		"tpl.menu.active": "ACTIVE",
		"tpl.menu.fullscreen": "Fullscreen",
		"tpl.menu.fullscreen.on": "on",
		"tpl.menu.fullscreen.off": "off",
		"tpl.menu.key.esc": "ESC",
		"tpl.menu.key.panels": "1–5",
		"tpl.menu.key.fullscreen": "F11",
		"tpl.menu.key.focus": "TAB",
		"tpl.menu.shortcut.esc": "Close overlays and open the menu",
		"tpl.menu.shortcut.panels": "Open panels 1 to 5",
		"tpl.menu.shortcut.fullscreen": "Toggle fullscreen",
		"tpl.menu.shortcut.focus": "Navigate with Tab and arrows; Enter activates",
		"hud.credits.body": "Open Grand Strategy — open-source economic simulation. Satellite imagery © Esri, Maxar, Earthstar Geographics.",
	},
	"es": {
		"tpl.common.page": "Página {page} de {pages}",
		"tpl.common.result": "{n} resultados",
		"tpl.footer.panels": "1–5 paneles",
		"tpl.footer.tabs": "Tab para navegar",
		"tpl.footer.close": "cerrar",
		"tpl.footer.table": "Haz clic en una fila para abrir la ficha · ↑↓ navegar",
		"tpl.footer.search": "/ buscar",
		"tpl.footer.filters": "F filtros",
		"tpl.footer.indicator": "I cambiar indicador",
		"tpl.footer.group": "G cambiar grupo",
		"tpl.footer.tabs_switch": "← → cambiar pestaña · ESC cerrar",

		"tpl.dossie.crumb": "Política › Dosier",
		"tpl.dossie.title": "Dosier de la nación",
		"tpl.dossie.seal": "CONFIDENCIAL · N.º {number}",
		"tpl.dossie.capital_line": "Capital · {city} · {government}",
		"tpl.dossie.gov.monarchy": "Monarquía parlamentaria",
		"tpl.dossie.cities": "Ciudades",
		"tpl.dossie.provinces": "Provincias",
		"tpl.dossie.ports": "Puertos",
		"tpl.dossie.area": "Territorio",
		"tpl.dossie.own": "TU NACIÓN",
		"tpl.dossie.open_economy": "Abrir economía",
		"tpl.dossie.view_map": "Ver en el mapa",
		"tpl.dossie.kpi.population": "Población",
		"tpl.dossie.kpi.gdp": "PIB (PPA)",
		"tpl.dossie.kpi.food": "Comida",
		"tpl.dossie.kpi.cash": "Caja del gobierno",
		"tpl.dossie.delta.population": "▲ 0,3% anual",
		"tpl.dossie.delta.gdp": "▲ 1,1%",
		"tpl.dossie.delta.food": "cobertura de 5 días",
		"tpl.dossie.delta.cash": "▼ 2,4% en el mes",
		"tpl.dossie.resources": "Recursos estratégicos",
		"tpl.dossie.relations": "Relaciones",
		"tpl.dossie.resources.note": "Autosuficiencia = producción ÷ consumo de los últimos 30 días.",
		"tpl.dossie.relations.note": "La diplomacia aún no se simula — estado fijo del escenario 2026.",
		"tpl.relation.ally": "Aliado",
		"tpl.relation.neutral": "Neutral",
		"tpl.relation.hostile": "Hostil",

		"tpl.registro.crumb": "Economía ›",
		"tpl.registro.title": "Registro de ciudades",
		"tpl.registro.search": "Buscar ciudad…",
		"tpl.registro.filter.all": "Todas · {n}",
		"tpl.registro.filter.deficit": "Con déficit · {n}",
		"tpl.registro.filter.ports": "Puertos · {n}",
		"tpl.registro.filter.capitals": "Capitales · {n}",
		"tpl.registro.col.city": "Ciudad",
		"tpl.registro.col.population": "Población",
		"tpl.registro.col.food": "Comida",
		"tpl.registro.col.stock": "Stock",
		"tpl.registro.col.deficit": "Déficit",
		"tpl.registro.col.deposits": "Yacimientos",
		"tpl.registro.selected": "Ficha seleccionada",
		"tpl.registro.tag.capital": "Capital",
		"tpl.registro.tag.deficit": "Déficit",
		"tpl.registro.tag.port": "Puerto: {port}",
		"tpl.registro.province": "Provincia",
		"tpl.registro.area": "Área administrada",
		"tpl.registro.employed": "Empleados",
		"tpl.registro.wage": "Salario medio",
		"tpl.registro.note": "El trigo lleva 6 días por debajo del consumo. Las importaciones de Róterdam llegan en 2 días.",
		"tpl.registro.open_sheet": "Abrir ficha completa",
		"tpl.registro.title.nations": "Registro de naciones",
		"tpl.registro.search.nation": "Buscar nación…",
		"tpl.registro.filter.transit": "Con tránsito · {n}",
		"tpl.registro.footer.select": "Haga clic para seleccionar · Enter o doble clic abre la ficha",
		"tpl.registro.hint.nations": "Doble clic en una nación abre su primera ciudad.",
		"tpl.registro.note.coverage": "El stock cubre {days} días de demanda.",
		"tpl.registro.note.short": "Demanda no atendida hoy: {amount}; el stock cubre {days} días.",
		"tpl.registro.note.empty": "Sin demanda registrada este día.",
		"tpl.registro.empty.nations": "La simulación aún no publicó naciones.",
		"tpl.registro.empty.nations_filtered": "Ninguna nación coincide con la búsqueda.",
		"tpl.registro.empty.nations_body": "Ajuste la búsqueda para ver la lista completa.",
		"tpl.registro.empty.cities": "La simulación aún no publicó ciudades.",
		"tpl.registro.empty.wait": "Los números aparecen cuando se publica el primer día.",
		"tpl.registro.demo.nation": "Reino Unido",
		"tpl.registro.demo.note": "El stock cubre 3,1 días de demanda.",

		"tpl.ficha.tab.overview": "Visión general",
		"tpl.ficha.tab.population": "Población",
		"tpl.ficha.tab.production": "Producción",
		"tpl.ficha.tab.market": "Mercado",
		"tpl.ficha.tab.resources": "Recursos",
		"tpl.ficha.crumb": "Reino Unido › Inglaterra › Greater London",
		"tpl.ficha.day": "DÍA {day}",
		"tpl.ficha.tag.capital": "Capital",
		"tpl.ficha.tag.port": "Puerto: Tilbury",
		"tpl.ficha.tag.provinces": "18 provincias",
		"tpl.ficha.stamp": "DÉFICIT DE TRIGO",
		"tpl.ficha.kpi.population": "Población",
		"tpl.ficha.kpi.employed": "Empleados",
		"tpl.ficha.kpi.gdp": "PIB local",
		"tpl.ficha.supply": "Abastecimiento · últimos 30 días",
		"tpl.ficha.supply.note": "Cobertura de comida (%) — rojo por debajo de 70%",
		"tpl.ficha.date.start": "4 dic",
		"tpl.ficha.date.end": "3 ene",
		"tpl.ficha.produces": "Produce",
		"tpl.ficha.consumes": "Consume",
		"tpl.ficha.industrial_goods": "Bienes industriales",
		"tpl.ficha.services": "Servicios",
		"tpl.ficha.import_wheat": "Importar trigo",
		"tpl.ficha.center": "Centrar en el mapa",
		"tpl.ficha.pending.title": "Aún no simulado",
		"tpl.ficha.pending.body": "Esta pestaña depende de datos que la simulación actual no publica. Aquí no se inventa nada.",
		"tpl.ficha.preview.title": "Vista previa de la pestaña",
		"tpl.ficha.preview.body": "Esta vista previa cubre la pestaña Visión general. El contenido de esta pestaña lo define la spec que use el generador.",

		"tpl.telegram.title": "TELEGRAMA",
		"tpl.telegram.meta": "N.º {number} · {status}",
		"tpl.telegram.urgent": "URGENTE",
		"tpl.telegram.from": "DE:",
		"tpl.telegram.to": "PARA:",
		"tpl.telegram.subject": "ASUNTO:",
		"tpl.telegram.from.value": "Ministerio de Abastecimiento",
		"tpl.telegram.to.value": "Gabinete del Primer Ministro",
		"tpl.telegram.subject.value": "Escasez de trigo en Londres",
		"tpl.telegram.body": "LAS EXISTENCIAS DE TRIGO EN LONDRES CAEN DESDE HACE SEIS DÍAS STOP COBERTURA DEL 66 POR CIENTO STOP PUERTO DE TILBURY CONGESTIONADO STOP ESPERAMOS INSTRUCCIONES STOP",
		"tpl.telegram.choice_label": "Elige una respuesta",
		"tpl.telegram.choice.import": "A · Importar trigo de emergencia",
		"tpl.telegram.choice.import.note": "−$ 1,2 mil millones de la caja · la cobertura vuelve al 85% en 3 días",
		"tpl.telegram.choice.ration": "B · Racionar el consumo",
		"tpl.telegram.choice.ration.note": "Consumo −15% durante 30 días · satisfacción −8",
		"tpl.telegram.choice.wait": "C · Esperar al mercado",
		"tpl.telegram.choice.wait.note": "Sin coste · riesgo de hambre si continúa el déficit",
		"tpl.telegram.footer": "Recibido {date} · el juego queda en pausa hasta la decisión",
		"tpl.telegram.received": "3 ene 2026 · 01:00",
		"tpl.telegram.stamp": "PRIORIDAD",

		"tpl.quadro.crumb": "Estadísticas ›",
		"tpl.quadro.title": "Cuadro comparativo",
		"tpl.quadro.indicator": "Indicador",
		"tpl.quadro.ind.population": "Población",
		"tpl.quadro.ind.gdp": "PIB",
		"tpl.quadro.ind.food": "Comida",
		"tpl.quadro.ind.minerals": "Reservas minerales",
		"tpl.quadro.group": "Grupo",
		"tpl.quadro.group.g7": "G7",
		"tpl.quadro.group.g7_select": "G7 ▾",
		"tpl.quadro.ranking": "Ranking · Población (millones)",
		"tpl.quadro.position": "Tu posición",
		"tpl.quadro.position.note": "de {group} en el {group_name} · {world}º de {world_total} en el mundo",
		"tpl.quadro.rank_suffix": "º",
		"tpl.quadro.above": "Arriba",
		"tpl.quadro.below": "Abajo",
		"tpl.quadro.above.value": "Alemania · +16 M",
		"tpl.quadro.below.value": "Francia · −2 M",
		"tpl.quadro.trend": "Tendencia · 12 meses",
		"tpl.quadro.source": "Fuente: escenario 2026 (ONU WPP). Tu nación en rojo.",
		"tpl.quadro.month.start": "ene 2025",
		"tpl.quadro.month.end": "ene 2026",

		"tpl.menu.title": "Open Grand Strategy",
		"tpl.menu.label": "ARCHIVO DEL GABINETE",
		"tpl.menu.label_meta": "DÍA {day}",
		"tpl.menu.paused": "— JUEGO EN PAUSA —",
		"tpl.menu.stamp": "EN PAUSA",
		"tpl.menu.save": "Guardar partida",
		"tpl.menu.load": "Cargar partida",
		"tpl.menu.soon": "Próximamente: el juego aún no tiene guardado.",
		"tpl.menu.hint.settings": "idioma · pantalla",
		"tpl.menu.hint.shortcuts": "teclado",
		"tpl.menu.shortcuts": "Atajos",
		"tpl.menu.quit": "Salir al escritorio",
		"tpl.menu.last_save": "Último guardado: {when}",
		"tpl.menu.version": "prealfa",
		"tpl.menu.back": "Volver",
		"tpl.menu.sheet.settings": "AJUSTES",
		"tpl.menu.sheet.shortcuts": "ATAJOS",
		"tpl.menu.sheet.credits": "CRÉDITOS",
		"tpl.menu.active": "ACTIVO",
		"tpl.menu.fullscreen": "Pantalla completa",
		"tpl.menu.fullscreen.on": "activada",
		"tpl.menu.fullscreen.off": "desactivada",
		"tpl.menu.key.esc": "ESC",
		"tpl.menu.key.panels": "1–5",
		"tpl.menu.key.fullscreen": "F11",
		"tpl.menu.key.focus": "TAB",
		"tpl.menu.shortcut.esc": "Cerrar superposiciones y abrir el menú",
		"tpl.menu.shortcut.panels": "Abrir los paneles 1 a 5",
		"tpl.menu.shortcut.fullscreen": "Alternar pantalla completa",
		"tpl.menu.shortcut.focus": "Navegar con Tab y flechas; Enter activa",
		"hud.credits.body": "Open Grand Strategy — simulación económica de código abierto. Imágenes de satélite © Esri, Maxar, Earthstar Geographics.",
	},
}

var STRINGS: Dictionary = {}

var locale: String = DEFAULT_LOCALE


func _ready() -> void:
	_load_strings()
	var saved := _load_saved_locale()
	if not saved.is_empty():
		set_locale(saved)


## Lê as traduções do JSON (gerado de data/definitions/i18n/i18n.csv).
func _load_strings() -> void:
	var file := FileAccess.open(I18N_PATH, FileAccess.READ)
	if file == null:
		push_error("I18n: não abriu %s" % I18N_PATH)
		return
	var data: Variant = JSON.parse_string(file.get_as_text())
	file.close()
	if typeof(data) == TYPE_DICTIONARY:
		STRINGS = data
	_merge_template_strings()


## Mescla os textos do gerador de telas sobre o JSON (sem reescrever o CSV).
func _merge_template_strings() -> void:
	for code in TEMPLATE_STRINGS:
		var table: Dictionary = STRINGS.get(code, {})
		var extra: Dictionary = TEMPLATE_STRINGS[code]
		for key in extra:
			table[key] = extra[key]
		STRINGS[code] = table
	if not STRINGS.has(DEFAULT_LOCALE):
		STRINGS[DEFAULT_LOCALE] = {}



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


## Grava o idioma escolhido em user://settings.cfg ([general] locale), para
## persistir entre sessões.
func save_locale(code: String) -> void:
	if not SUPPORTED_LOCALES.has(code):
		return
	var config := ConfigFile.new()
	config.set_value("general", "locale", code)
	var err := config.save(SETTINGS_PATH)
	if err != OK:
		push_warning("I18n: falhou ao gravar %s (erro %d)" % [SETTINGS_PATH, err])


## Lê o idioma salvo em disco; vazio se ainda não existir.
func _load_saved_locale() -> String:
	var config := ConfigFile.new()
	if config.load(SETTINGS_PATH) != OK:
		return ""
	return str(config.get_value("general", "locale", ""))


func short_label() -> String:
	return SHORT_LABELS.get(locale, "PT")


## Nome localizado de uma nação a partir da tag (GBR, FRA...). O contrato novo
## do plano é `nation.name.*`; `nation.*` fica como alias legado dos saves/telas
## antigas.
func nation_name(tag: String) -> String:
	var key := "nation.name.%s" % tag
	var value := t(key)
	return value if value != key else t("nation.%s" % tag)


## Nome localizado de uma cidade a partir do id (GBR-LDN...).
func city_name(city_id: String, fallback: String) -> String:
	var key := "city.name.%s" % city_id
	var value := t(key)
	return fallback if value == key else value


## Nome localizado de um porto a partir do id (GBR-DOV...).
func port_name(port_id: String, fallback: String) -> String:
	var key := "port.name.%s" % port_id
	var value := t(key)
	return fallback if value == key else value
