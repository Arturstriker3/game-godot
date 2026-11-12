"""Curadoria explícita de nações do cenário mundial (fonte única).

Este módulo é lido por `tools/build_world_cities.py` (roster do cenário),
`tools/build_atlas_layers.py` (dono das províncias no raster), por
`tools/generate_nav_mask.py` (terra que virou mar) e por
`tools/build_province_nature.py` (dono dos depósitos de territórios apagados).
A decisão fica aqui, e não numa planilha, para que uma regeneração completa
(importador → build) não reintroduza uma nação removida nem perca uma fusão.

Critério (decisão 2026-11, aprovada pelo usuário)
--------------------------------------------------
Uma nação SOBERANA fica no cenário se `population >= 500.000` OU
`area_km2 >= 10.000`. Ficam sempre as exceções estratégicas
`STRATEGIC_KEEP` (Singapura, Bahrein, Catar, Brunei, Luxemburgo, Malta,
Chipre, Djibuti), mesmo abaixo do limiar.

Rodada r2 (2026-11-12): o usuário devolveu as pequenas nações do Caribe
(Barbados, Granada, São Vicente e Granadinas, Santa Lúcia, Antígua e Barbuda,
Dominica, São Cristóvão e Névis) como nações jogáveis — exceção estratégica
regional, apesar de todas ficarem abaixo do limiar. Aruba e Curaçao continuam
territórios e são fundidos na Holanda (NED). As Malvinas voltam como território
britânico (GBR) com a cidade de Stanley. Maurício e a Polinésia Francesa saem
por inteiro e passam a apagados.

Rodada r3 (2026-11-12): Guam volta como território dos EUA (mesmo destino da
r1: fundido em USA, com Hagåtña e porto em Apra Harbor). A ilha de Okinawa
(Naha) e Miyakojima voltam ao mapa como cidades japonesas; as três são mantidas
no atlas e no nav_mask pelos seeds de `build_atlas_layers.py`.

Microestados europeus não viram mar nem nação própria: o território é
ANEXADO ao vizinho e as províncias passam a ser do vizinho.

Ilhas soberanas abaixo do limiar (sem fronteira terrestre) são REMOVIDAS:
cidades, portos, construções e a própria nação saem; no atlas e no nav_mask a
terra delas vira mar. A decisão do usuário remove também quatro ilhas que
passariam pelo critério por área/população (Vanuatu, Maldivas, Comores, Cabo
Verde): ilha pequena sem hinterlândia continental não paga o custo de mapa.

Territórios dependentes seguem a mesma lógica, com dois destinos:
  * GRANDES (pop >= 500.000, area >= 10.000 km² ou nomeados pelo usuário) são
    FUNDIDOS na nação-mãe — províncias, cidades e portos passam à mãe;
  * MINÚSCULOS são APAGADOS (a terra vira mar). Inclui os citados pelo usuário
    (Gibraltar, Bermudas, Malvinas, Ilhas Virgens) e os demais abaixo do
    limiar sem relevância cartográfica.

Onde há conflito, vale a LISTA EXPLÍCITA do usuário; o critério serve para
provar que nada ficou de fora. `audit_source()` falha se uma nação abaixo do
limiar não estiver classificada, se um território não tiver destino, se uma
tag não existir na fonte ou se um tag tiver dois destinos.
"""

from __future__ import annotations

# Limiar do critério (não é número de jogo: é a regra de curadoria aprovada).
KEEP_POP = 500_000
KEEP_AREA_KM2 = 10_000.0

# Exceções estratégicas: ficam mesmo abaixo do limiar (pop/área).
STRATEGIC_KEEP = {
    "SGP": "cidade-estado; centro financeiro e porto global",
    "BHR": "monarquia do Golfo; base naval e elo Arábia–Ásia",
    "QAT": "monarquia do Golfo; maior campo de gás do mundo (North Field)",
    "BRN": "sultanato de Bornéu; petróleo e posição no mar da China Meridional",
    "LUX": "centro financeiro europeu; membro fundador da UE",
    "MLT": "ilha estratégica no Mediterrâneo central",
    "CYP": "ilha do Mediterrâneo oriental; porta do Levante",
    "DJI": "chifre da África; corredor do mar Vermelho",
    # Exceção estratégica regional do Caribe (decisão r2): ilhas pequenas,
    # mas jogáveis por escolha do usuário. As Malvinas não entram aqui: são
    # território britânico fundido em GBR, não nação própria.
    "BRB": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "GRD": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "VCT": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "LCA": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "ATG": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "DMA": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
    "KNA": "exceção estratégica Caribe; ilha jogável por decisão do usuário",
}

# Microestados europeus: (tag da fonte) -> (tag do vizinho que recebe).
# O território e as províncias passam ao vizinho; a nação desaparece.
MICROSTATES = {
    "VAT": "ITA",   # Vaticano -> Itália (enclave em Roma)
    "SMR": "ITA",   # San Marino -> Itália (enclave)
    "MCO": "FRA",   # Mônaco -> França
    "LIE": "CHE",   # Liechtenstein -> Suíça (fronteira terrestre com CHE/AUT)
    "AND": "ESP",   # Andorra -> Espanha (fronteira terrestre com ESP/FRA)
}

# Ilhas soberanas removidas por inteiro (sem fronteira terrestre).
# A lista abaixo é a do pedido do usuário; as quatro últimas passariam pelo
# critério por área/população e saem por decisão explícita (ilha sem
# hinterlândia; o custo de mapa não se paga).
REMOVED_SOVEREIGNS = {
    "MUS": "Maurício",
    "TUV": "Tuvalu",
    "NRU": "Nauru",
    "KIR": "Kiribati",
    "MHL": "Ilhas Marshall",
    "PLW": "Palau",
    "FSM": "Micronésia",
    "TON": "Tonga",
    "WSM": "Samoa",
    "STP": "São Tomé e Príncipe",
    "SYC": "Seicheles",
    "VUT": "Vanuatu",
    "MDV": "Maldivas",
    "COM": "Comores",
    "CPV": "Cabo Verde",
}

# Removidas por decisão explícita apesar de passarem no critério (o relatório
# marca a exceção; não é um número inventado, é uma decisão do usuário).
EXPLICIT_REMOVE_OVERRIDE = {"MUS", "VUT", "MDV", "COM", "CPV"}

# Territórios dependentes: nação-mãe de cada um (todos os 34 da fonte).
# A tabela serve de fallback de dono para depósitos e de documentação.
TERRITORY_MOTHER = {
    "GRL": "DNK", "FRO": "DNK",
    "PRI": "USA", "GUM": "USA", "ASM": "USA", "MNP": "USA", "VIR": "USA",
    "NCL": "FRA", "PYF": "FRA", "WLF": "FRA", "SPM": "FRA",
    "MAF": "FRA", "BLM": "FRA",
    "HKG": "CHN", "MAC": "CHN",
    "CUW": "NED", "ABW": "NED", "SXM": "NED",
    "BMU": "GBR", "CYM": "GBR", "TCA": "GBR", "VGB": "GBR", "AIA": "GBR",
    "MSR": "GBR", "SHN": "GBR", "FLK": "GBR", "GIB": "GBR", "IMN": "GBR",
    "JEY": "GBR", "GGY": "GBR",
    "COK": "NZL", "NIU": "NZL",
    "NFK": "AUS", "ALD": "FIN",
}

# Territórios grandes: fundidos na nação-mãe (território + províncias + cidades).
MERGED_TERRITORIES = {
    "GRL": "DNK",  # Groenlândia (2,16 M km²)
    "PRI": "USA",  # Porto Rico (3,2 M hab.)
    "NCL": "FRA",  # Nova Caledônia (18.575 km²)
    "HKG": "CHN",  # Hong Kong (7,5 M hab.)
    "MAC": "CHN",  # Macau (0,7 M hab.)
    "FLK": "GBR",  # Malvinas (r2: território britânico, Stanley no cenário)
    "CUW": "NED",  # Curaçao (r2: território holandês)
    "ABW": "NED",  # Aruba (r2: território holandês)
    "GUM": "USA",  # Guam (r3: devolvido como território dos EUA, Hagåtña no cenário)
}

# Territórios minúsculos removidos: a terra vira mar.
REMOVED_TERRITORIES = {
    "GIB": "Gibraltar",
    "BMU": "Bermudas",
    "VIR": "Ilhas Virgens Americanas",
    "VGB": "Ilhas Virgens Britânicas",
    "FRO": "Ilhas Faroé",
    "ASM": "Samoa Americana",
    "MNP": "Marianas do Norte",
    "PYF": "Polinésia Francesa",
    "SXM": "Sint Maarten",
    "CYM": "Cayman",
    "TCA": "Turcas e Caicos",
    "AIA": "Anguilla",
    "MSR": "Montserrat",
    "SHN": "Santa Helena",
    "COK": "Ilhas Cook",
    "NIU": "Niue",
    "WLF": "Wallis e Futuna",
    "SPM": "São Pedro e Miquelon",
    "MAF": "São Martinho (FR)",
    "BLM": "São Bartolomeu",
    "NFK": "Norfolk",
    "IMN": "Ilha de Man",
    "JEY": "Jersey",
    "GGY": "Guernsey",
    "ALD": "Åland",
}

# Áreas admin-0 do Natural Earth que não são nações da fonte e não têm cidade
# neste cenário: terra sem dono no mapa (bege). Saem do atlas e da natureza
# como uma ilha remota — nunca têm cidade, porto ou jazida no cenário.
# (ATF = Terras Austrais e Antárticas Francesas: Kerguelen, Crozet, Amsterdã,
# Santo Paulo e a Terra Adélia; SGS = Geórgia do Sul e Sandwich do Sul.)
REMOVED_UNOWNED_AREAS = {
    "ATF": "Terras Austrais e Antárticas Francesas (Kerguelen e ilhas)",
    "SGS": "Geórgia do Sul e Sandwich do Sul",
}

# Decisões explícitas acima do limiar: o relatório tem de declará-las.
EXPLICIT_OVERRIDES = {
    "PYF": "Pedido do usuário (Polinésia Francesa): apagar por inteiro",
    **{tag: "Pedido do usuário: ilha sem hinterlândia, apagar apesar do critério"
       for tag in sorted(EXPLICIT_REMOVE_OVERRIDE)},
}

# Todos os destinos (nação-mãe) por tag de origem: fusão + anexação.
TRANSFERS = {**MICROSTATES, **MERGED_TERRITORIES}
# Toda tag que deixa de existir como nação.
REMOVED = set(MICROSTATES) | set(REMOVED_SOVEREIGNS) | set(REMOVED_TERRITORIES) \
    | set(MERGED_TERRITORIES)


def transfer_target(tag: str) -> str:
    """Nação que recebe o território de `tag`, ou "" se não há transferência."""
    return TRANSFERS.get(tag, "")


def atlas_owner_map() -> dict:
    """ADM0_A3 do Natural Earth -> ADM0_A3 do dono no raster do atlas.

    O roster usa NED para a Holanda, mas o admin-0 do Natural Earth é NLD; a
    fusão de Aruba/Curaçao precisa gravar NLD no raster (o jogo remapeia NLD
    para NED em `write_provinces_json`).
    """
    overrides = {"NED": "NLD"}
    return {tag: overrides.get(target, target) for tag, target in TRANSFERS.items()}


def atlas_removed() -> set:
    """ADM0_A3 removidos do raster (terra vira mar).

    Inclui as áreas sem dono (ATF/SGS) que não existem na fonte de nações:
    são terra sem cidade, sem porto e sem jazida no cenário. A remoção é a
    mesma das demais ilhas — não há transferência para nação-mãe.
    """
    return set(REMOVED_SOVEREIGNS) | set(REMOVED_TERRITORIES) \
        | set(REMOVED_UNOWNED_AREAS)


def classify(tag: str) -> tuple:
    """(destino, tag_alvo) — destino em kept|annexed|merged|removed."""
    if tag in MICROSTATES:
        return "annexed", MICROSTATES[tag]
    if tag in MERGED_TERRITORIES:
        return "merged", MERGED_TERRITORIES[tag]
    if tag in REMOVED_SOVEREIGNS or tag in REMOVED_TERRITORIES:
        return "removed", ""
    return "kept", tag


def below_threshold(row) -> bool:
    return (float(row.get("population") or 0) < KEEP_POP
            and float(row.get("area_km2") or 0) < KEEP_AREA_KM2)


def audit_source(rows) -> tuple:
    """Valida a curadoria contra a fonte de nações.

    Devolve (linhas_do_relatorio, erros). Uma fonte sem nenhum erro e sem
    nenhuma nação abaixo do limiar sem destino é o contrato do módulo.
    """
    by_tag = {}
    for row in rows:
        tag = (row.get("iso3") or "").strip()
        if tag:
            by_tag[tag] = row

    errors = []
    report = []

    # 1. Nenhuma tag em dois destinos.
    groups = (set(MICROSTATES), set(MERGED_TERRITORIES),
              set(REMOVED_SOVEREIGNS), set(REMOVED_TERRITORIES))
    for tag in sorted(set().union(*groups)):
        count = sum(1 for group in groups if tag in group)
        if count != 1:
            errors.append(f"{tag}: {count} destinos na curadoria (esperado 1)")

    # 2. Toda tag da curadoria existe na fonte e tem tipo compatível.
    for tag in sorted(REMOVED | set(TRANSFERS)):
        row = by_tag.get(tag)
        if row is None:
            errors.append(f"{tag}: curadoria cita nação ausente da fonte")
            continue
        status = (row.get("status") or "").strip()
        if tag in TERRITORY_MOTHER and status != "territory":
            errors.append(f"{tag}: curadoria trata como território, fonte diz {status}")
        if (tag in MICROSTATES or tag in REMOVED_SOVEREIGNS) and status == "territory":
            errors.append(f"{tag}: curadoria trata como soberana, fonte diz território")

    # 3. Nenhuma nação abaixo do limiar fica de fora (exceto exceções).
    for tag, row in sorted(by_tag.items()):
        if not below_threshold(row):
            continue
        if tag in STRATEGIC_KEEP:
            continue
        if tag not in REMOVED:
            errors.append(f"{tag}: abaixo do limiar ({int(float(row['population'] or 0))} hab., "
                          f"{float(row['area_km2'] or 0):.1f} km²) sem destino na curadoria")

    # 4. Nenhum território fica sem destino (todos os 34 da fonte).
    for tag, row in sorted(by_tag.items()):
        if (row.get("status") or "").strip() != "territory":
            continue
        if tag not in MERGED_TERRITORIES and tag not in REMOVED_TERRITORIES:
            errors.append(f"{tag}: território sem destino (fundir ou apagar)")

    # 5. Relatório (todas as tags afetadas, com os números da fonte).
    for tag in sorted(REMOVED | set(TRANSFERS)):
        row = by_tag.get(tag)
        if row is None:
            continue
        action, target = classify(tag)
        report.append({
            "tag": tag,
            "name": row.get("name_en", ""),
            "status": (row.get("status") or "").strip(),
            "population": int(float(row.get("population") or 0)),
            "area_km2": float(row.get("area_km2") or 0),
            "action": action,
            "target": target,
            "below_threshold": below_threshold(row),
            "override": EXPLICIT_OVERRIDES.get(tag, ""),
        })

    # 6. O relatório precisa declarar as exceções acima do limiar.
    for tag in sorted(EXPLICIT_OVERRIDES):
        row = by_tag.get(tag)
        if row is None:
            errors.append(f"{tag}: exceção explícita sem linha na fonte")
        elif tag not in REMOVED:
            errors.append(f"{tag}: exceção explícita sem remoção")
        elif not EXPLICIT_OVERRIDES[tag]:
            errors.append(f"{tag}: exceção explícita sem justificativa")

    return report, errors


def report_markdown(rows) -> list:
    """Tabela markdown do relatório (para docs/plans/world_all_data.md)."""
    report, errors = audit_source(rows)
    if errors:
        raise SystemExit("curadoria de nações inválida: " + "; ".join(errors))
    lines = [
        "| tag | nação | tipo | pop | área km² | ação | destino | nota |",
        "|---|---|---|---:|---:|---|---|---|",
    ]
    for item in report:
        lines.append("| {tag} | {name} | {status} | {population} | {area:.1f} | "
                     "{action} | {target} | {override} |".format(
                         tag=item["tag"], name=item["name"], status=item["status"],
                         population=item["population"], area=item["area_km2"],
                         action=item["action"], target=item["target"] or "—",
                         override=item["override"] or ""))
    return lines
