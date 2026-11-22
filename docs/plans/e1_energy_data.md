# E1 — Dados de energia: coleta antecipada (movido)

Este documento foi promovido e reescrito como **`docs/economy/E1_energy_data.md`**
(fontes e licenças verificadas, matriz nacional 2024–2025 da Ember, parque base GPPD
~2021 + parque escalonado 2026, potencial solar/eólico GSA/GWA, potencial hídrico
CHIRPS/PET/ETOPO1, validador e lacunas).

**Decisão do dono D-115 (2026-11-21):** (a) a usina pertence ao controlador da
cidade/província no mapa do jogo (`nation` = atlas; `gppd_country` preservado);
(b) parque inicial = GPPD ~2021 escalonado por nação×fonte até a Ember 2024–2025
(`power_plants_base_2021_gppd.json` + `power_plants_2026_scaled.json`);
(c) solar/eólico com Global Solar Atlas/Global Wind Atlas (CC BY 4.0) e hídrico com
CHIRPS/CHELSA PET (CC0 1.0)/ETOPO1 (sem WorldClim); (d) óleo tentado com OWID/EIA, mas nenhum
publica capacidade instalada de óleo por país — lacuna declarada.

O conteúdo anterior da coleta (GPPD/WorldClim/WDI/IRENA, comparação com a IRENA e
fronteiras) continua no histórico do git; a mudança de dado em relação a ele é a saída
do WorldClim e da IRENA das saídas versionadas, o escalonamento declarado e a
classificação dos conflitos de localização.

Plano: `docs/plans/perf_pop_financas_plano.md`, PARTE E, E1. Branch `data/e1-energy`.
