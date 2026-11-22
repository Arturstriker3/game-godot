"""Valida o patch B3 das 15 capitais extras (D-005, opcao 3).

Contexto: `docs/plans/b3_extra_capitals.md` (D-005 opcao 3, ver
`GAME_STATE.md`). Cada estado admin-1 com populacao 2026 acima de 20 milhoes
que o B3 anexou a um vizinho (`attach_rule=neighbor_largest_population`) ganha
a sua capital administrativa real como cidade propria. Este script confere o
resultado no `state_regions.json` e no patch
`data/definitions/scenario/extra_capitals_b3.json`, sem comparar com uma
segunda copia e sem reescrever nada.

Fonte de cada grandeza:

  * populacao de estado e de regiao (WorldPop 2026 rateado por provincia
    admin-1; total 8.203.501.062):
    `data/definitions/population/state_regions.json`;
  * populacao nacional: `data/definitions/scenario/nations.json`;
  * cidade propria e coordenadas: GeoNames `cities15000` (dump 2026,
    https://download.geonames.org/export/dump/cities15000.zip), geonameid
    registrado em `cities[*].provenance`;
  * aglomeracao / populacao urbana: `tools/agglomerations_2026.csv`
    (Thomas Brinkhoff, citypopulation.de, ref. 2026-01-01);
  * `population` das cidades: valor DERIVADO do rateio da populacao nacional
    por peso `urban_population` (mesma regra de
    `tools/build_world_cities.py:distribute_national_population`, aqui
    reimplementada para conferir).

O script e somente leitura: nao grava arquivo. Sai com 0 quando todos os
checks passam e com 1 quando algum falha, listando todos os problemas.

Uso (Python 3.9+, stdlib apenas):

    G:/tools/cpp-tools/Scripts/python.exe tools/validate_extra_capitals_b3.py
"""

import json
import os
import sys
import time

AQUI = os.path.dirname(os.path.abspath(__file__))
RAIZ = os.path.dirname(AQUI)

ARQ_REGIOES = os.path.join(RAIZ, "data", "definitions", "population",
                           "state_regions.json")
ARQ_CIDADES = os.path.join(RAIZ, "data", "definitions", "scenario",
                           "cities.json")
ARQ_NACOES = os.path.join(RAIZ, "data", "definitions", "scenario",
                          "nations.json")
ARQ_EXTRA = os.path.join(RAIZ, "data", "definitions", "scenario",
                         "extra_capitals_b3.json")
ARQ_I18N = os.path.join(RAIZ, "data", "definitions", "i18n", "i18n.json")

TOTAL_ESPERADO = 8_203_501_062
ESTADOS_ESPERADOS = 4226
CIDADES_ESPERADAS = 849
CIDADES_NOVAS_ESPERADAS = 15
LIMIAR_GRANDE = 20_000_000
ABAIXO_URBANO_ESPERADAS = 19
TOLERANCIA_COORD = 0.05
TOLERANCIA_NUM = 1e-6
MAPA_LARGURA = 5000.0
MAPA_ALTURA = 2500.0
LOCALES = ("pt_BR", "en", "es")
FONTES_URBANO = ("agglomeration", "agglomeration_capped", "sim_population")


def carregar(caminho):
    with open(caminho, "r", encoding="utf-8") as arquivo:
        return json.load(arquivo)


def eh_numero(valor):
    return isinstance(valor, (int, float)) and not isinstance(valor, bool)


def eh_inteiro(valor):
    return isinstance(valor, int) and not isinstance(valor, bool)


def iguais(a, b, tolerancia=TOLERANCIA_NUM):
    return abs(float(a) - float(b)) <= tolerancia


def formatar(valor):
    return "{:,}".format(int(round(float(valor)))).replace(",", ".")


def lonlat_para_xy(lon, lat):
    return (round((lon + 180.0) / 360.0 * MAPA_LARGURA, 1),
            round((90.0 - lat) / 180.0 * MAPA_ALTURA, 1))


def distribuir_populacao_nacional(cidades, populacao_por_nacao):
    """Regra de `build_world_cities.distribute_national_population`.

    Peso = `urban_population`; a soma por nacao e exata (resto pelas maiores
    fracoes, desempate pelo id crescente). Devolve {city_id: population}.
    """
    por_nacao = {}
    for cidade in cidades:
        por_nacao.setdefault(cidade["nation"], []).append(cidade)

    resultado = {}
    for nacao, grupo in por_nacao.items():
        total = int(populacao_por_nacao.get(nacao, 0))
        pesos = [max(0, int(cidade["urban_population"])) for cidade in grupo]
        soma_pesos = sum(pesos)
        if total <= 0:
            for cidade in grupo:
                resultado[cidade["id"]] = int(cidade.get("population", 0) or 0)
            continue
        if soma_pesos <= 0:
            base, sobra = divmod(total, len(grupo))
            for indice, cidade in enumerate(grupo):
                resultado[cidade["id"]] = base + (1 if indice < sobra else 0)
            continue
        brutos = [total * peso / soma_pesos for peso in pesos]
        valores = [int(bruto) for bruto in brutos]
        resto = total - sum(valores)
        ordem = sorted(range(len(grupo)),
                       key=lambda i: (-(brutos[i] - valores[i]),
                                      grupo[i]["id"]))
        for indice in ordem[:resto]:
            valores[indice] += 1
        for cidade, valor in zip(grupo, valores):
            resultado[cidade["id"]] = valor
    return resultado


class Validador(object):
    def __init__(self, regioes, cidades_json, nacoes_json, extra, i18n_json):
        self.estados = regioes.get("states", {})
        self.cidades_regiao = regioes.get("cities", {})
        self.cidades_json = cidades_json
        self.nacoes_json = nacoes_json
        self.extra = extra
        self.i18n_json = i18n_json

        self.novas = extra.get("cities", []) or []
        self.novas_por_id = {}
        for cidade in self.novas:
            self.novas_por_id[cidade.get("id")] = cidade
        self.ids_novos = [cidade.get("id") for cidade in self.novas]
        self.patch_estado = extra.get("state_patch", {}) or {}
        self.patch_vizinho = extra.get("neighbor_patch", {}) or {}
        self.rateio_extra = extra.get("population_rateio_after", {}) or {}
        self.i18n_extra = extra.get("i18n", {}) or {}

        self.cidades_por_id = {cidade.get("id"): cidade
                               for cidade in cidades_json}
        self.cidades_por_id.update(self.novas_por_id)
        self.populacao_nacao = {nacao.get("id"): int(nacao.get("population", 0))
                                for nacao in nacoes_json}

        self.problemas = []
        self.resumo = []
        self.fecham = 0
        self.total_nacoes = 0
        self.aberto = 0
        self.aberto_urbano = 0
        self.cobertura_estados = 0

    # ------------------------------------------------------------------ util

    def problema(self, etiqueta, mensagem):
        self.problemas.append("[%s] %s" % (etiqueta, mensagem))

    def exigir(self, condicao, etiqueta, mensagem):
        if not condicao:
            self.problema(etiqueta, mensagem)
        return condicao

    # --------------------------------------------------------------- checks

    def validar_soma(self):
        etiqueta = "SOMA"
        soma_estados = sum(estado.get("population_2026", 0)
                           for estado in self.estados.values())
        soma_regioes = sum(cidade.get("population_region", 0)
                           for cidade in self.cidades_regiao.values())
        self.exigir(soma_estados == TOTAL_ESPERADO, etiqueta,
                    "soma dos estados %s != %s"
                    % (formatar(soma_estados), formatar(TOTAL_ESPERADO)))
        self.exigir(soma_regioes == TOTAL_ESPERADO, etiqueta,
                    "soma de population_region %s != %s"
                    % (formatar(soma_regioes), formatar(TOTAL_ESPERADO)))

        estados_por_nacao = {}
        for estado in self.estados.values():
            nacao = estado.get("nation")
            estados_por_nacao[nacao] = (estados_por_nacao.get(nacao, 0)
                                        + estado.get("population_2026", 0))

        regioes_por_nacao = {}
        sem_nacao = []
        for city_id, cidade in self.cidades_regiao.items():
            registro = self.cidades_por_id.get(city_id)
            if registro is None or not registro.get("nation"):
                sem_nacao.append(city_id)
                continue
            nacao = registro["nation"]
            regioes_por_nacao[nacao] = (regioes_por_nacao.get(nacao, 0)
                                        + cidade.get("population_region", 0))
        if sem_nacao:
            self.problema(etiqueta, "%d cidades de regiao sem nacao em "
                                    "cities.json/extra: %s"
                          % (len(sem_nacao), ", ".join(sorted(sem_nacao)[:8])))

        self.total_nacoes = len(self.populacao_nacao)
        for nacao in sorted(self.populacao_nacao):
            esperado = self.populacao_nacao[nacao]
            soma_e = estados_por_nacao.get(nacao, 0)
            soma_r = regioes_por_nacao.get(nacao, 0)
            if soma_e == soma_r == esperado:
                self.fecham += 1
                continue
            self.problema(
                etiqueta,
                "nacao %s: estados=%s regioes=%s nations.json=%s"
                % (nacao, formatar(soma_e), formatar(soma_r),
                   formatar(esperado)))

        orfas = sorted(set(estados_por_nacao) - set(self.populacao_nacao))
        for nacao in orfas:
            soma_e = estados_por_nacao.get(nacao, 0)
            soma_r = regioes_por_nacao.get(nacao, 0)
            if soma_e or soma_r:
                self.problema(etiqueta,
                              "nacao %s tem estados (%s) ou regioes (%s) mas "
                              "nao existe em nations.json"
                              % (nacao, formatar(soma_e), formatar(soma_r)))
            else:
                self.resumo.append(
                    "aviso: tag de nacao %s so em state_regions (0 hab., sem "
                    "cidade de regiao)" % nacao)

    def validar_estados_novos(self):
        etiqueta = "OS15"
        atendidos = 0
        for state_id, combinado in sorted(self.patch_estado.items()):
            city_id = combinado.get("city_id")
            estado = self.estados.get(state_id)
            cidade = self.cidades_regiao.get(city_id)
            nova = self.novas_por_id.get(city_id)
            if estado is None:
                self.problema(etiqueta, "estado %s inexistente" % state_id)
                continue
            if cidade is None:
                self.problema(etiqueta,
                              "estado %s aponta para cidade %s ausente de "
                              "state_regions.json" % (state_id, city_id))
                continue
            if nova is None:
                self.problema(etiqueta,
                              "estado %s aponta para %s, que nao esta em "
                              "extra['cities']" % (state_id, city_id))
                continue
            ok = True
            ok &= self.exigir(estado.get("city_id") == city_id, etiqueta,
                              "%s: city_id do estado = %r, patch pede %r"
                              % (state_id, estado.get("city_id"), city_id))
            ok &= self.exigir(estado.get("attach_rule") == "primary_city",
                              etiqueta,
                              "%s: attach_rule = %r (esperado primary_city)"
                              % (state_id, estado.get("attach_rule")))
            ok &= self.exigir(cidade.get("states") == [state_id], etiqueta,
                              "%s: %s.states = %r (esperado [%r])"
                              % (state_id, city_id, cidade.get("states"),
                                 state_id))
            ok &= self.exigir(
                iguais(cidade.get("population_region", -1),
                       estado.get("population_2026", -2)), etiqueta,
                "%s: %s.population_region = %r != population_2026 %r"
                % (state_id, city_id, cidade.get("population_region"),
                   estado.get("population_2026")))
            ok &= self.exigir(
                cidade.get("urban_population") == nova.get("urban_population"),
                etiqueta,
                "%s: %s.urban_population = %r != extra %r"
                % (state_id, city_id, cidade.get("urban_population"),
                   nova.get("urban_population")))
            if ok:
                atendidos += 1
        self.cobertura_estados = atendidos
        self.exigir(atendidos == CIDADES_NOVAS_ESPERADAS, etiqueta,
                    "cobertura %d/%d estados atendidos"
                    % (atendidos, CIDADES_NOVAS_ESPERADAS))

    def validar_criterio(self):
        etiqueta = "CRITERIO"
        grandes_sem_cidade = {state_id for state_id, estado
                              in self.estados.items()
                              if estado.get("population_2026", 0) > LIMIAR_GRANDE
                              and estado.get("attach_rule") != "primary_city"}
        if grandes_sem_cidade:
            self.problema(etiqueta,
                          "%d estados >20M sem cidade propria: %s"
                          % (len(grandes_sem_cidade),
                             ", ".join(sorted(grandes_sem_cidade))))
        self.aberto = len(grandes_sem_cidade)

        ids_novos = set(self.ids_novos)
        esperado = {state_id for state_id, estado in self.estados.items()
                    if estado.get("population_2026", 0) > LIMIAR_GRANDE
                    and estado.get("city_id") in ids_novos}
        informado = set(self.patch_estado)
        if informado != esperado:
            self.problema(etiqueta,
                          "state_patch difere do criterio: faltam %s ; sobram %s"
                          % (sorted(esperado - informado),
                             sorted(informado - esperado)))
        if len(self.patch_estado) != CIDADES_NOVAS_ESPERADAS:
            self.problema(etiqueta, "state_patch tem %d estados (esperado %d)"
                          % (len(self.patch_estado),
                             CIDADES_NOVAS_ESPERADAS))

    def validar_ids(self):
        etiqueta = "IDS"
        self.exigir(len(self.novas) == CIDADES_NOVAS_ESPERADAS, etiqueta,
                    "extra['cities'] tem %d cidades (esperado %d)"
                    % (len(self.novas), CIDADES_NOVAS_ESPERADAS))
        self.exigir(len(set(self.ids_novos)) == len(self.ids_novos), etiqueta,
                    "ids repetidos no proprio extra: %s"
                    % sorted(cid for cid in set(self.ids_novos)
                             if self.ids_novos.count(cid) > 1))

        existentes = {cidade.get("id") for cidade in self.cidades_json}
        for city_id in self.ids_novos:
            self.exigir(city_id not in existentes, etiqueta,
                        "id %s ja existe em cities.json" % city_id)
            self.exigir(city_id in self.cidades_regiao, etiqueta,
                        "id %s ausente de state_regions['cities']" % city_id)

        aparicoes = {}
        for cidade in self.novas:
            prov = cidade.get("provenance") or {}
            state_id = prov.get("state_id")
            aparicoes[state_id] = aparicoes.get(state_id, 0) + 1
        for state_id, quantidade in sorted(aparicoes.items()):
            self.exigir(quantidade == 1, etiqueta,
                        "estado %s aparece em %d cidades novas"
                        % (state_id, quantidade))
        self.exigir(len(aparicoes) == CIDADES_NOVAS_ESPERADAS, etiqueta,
                    "cidades novas apontam para %d estados distintos "
                    "(esperado %d)" % (len(aparicoes),
                                       CIDADES_NOVAS_ESPERADAS))

    def validar_campos(self):
        etiqueta = "CAMPOS"
        contratos = (
            ("id", str),
            ("nation", str),
            ("home_nation", str),
            ("name", str),
            ("population", "numero"),
            ("sim_population", "numero"),
            ("agglomeration_population", ("inteiro", None)),
            ("urban_population", int),
            ("urban_population_source", str),
            ("production", "numero"),
            ("x", "numero"),
            ("y", "numero"),
            ("landmass", str),
            ("capital", bool),
        )
        for cidade in self.novas:
            city_id = cidade.get("id")
            for campo, tipo in contratos:
                if campo not in cidade:
                    self.problema(etiqueta,
                                  "%s: campo %s ausente" % (city_id, campo))
                    continue
                valor = cidade[campo]
                if tipo == "numero":
                    valido = eh_numero(valor)
                elif isinstance(tipo, tuple):
                    valido = eh_inteiro(valor) or valor is None
                elif tipo is int:
                    valido = eh_inteiro(valor)
                elif tipo is bool:
                    valido = isinstance(valor, bool)
                else:
                    valido = isinstance(valor, tipo)
                if not valido:
                    self.problema(
                        etiqueta, "%s: %s = %r (%s), esperado %s"
                        % (city_id, campo, valor, type(valor).__name__,
                           "inteiro ou null" if isinstance(tipo, tuple)
                           else getattr(tipo, "__name__", tipo)))
            fonte = cidade.get("urban_population_source")
            if fonte not in FONTES_URBANO:
                self.problema(etiqueta, "%s: urban_population_source = %r"
                              % (city_id, fonte))
            for campo in ("sim_population", "urban_population"):
                valor = cidade.get(campo)
                if eh_numero(valor) and valor <= 0:
                    self.problema(etiqueta,
                                  "%s: %s = %r (deve ser > 0)"
                                  % (city_id, campo, valor))

    def validar_coordenadas(self):
        etiqueta = "COORD"
        for cidade in self.novas:
            city_id = cidade.get("id")
            prov = cidade.get("provenance") or {}
            lon = prov.get("lon")
            lat = prov.get("lat")
            if not eh_numero(lon) or not eh_numero(lat):
                self.problema(etiqueta,
                              "%s: provenance sem lat/lon (%r/%r)"
                              % (city_id, lat, lon))
                continue
            esperado_x, esperado_y = lonlat_para_xy(float(lon), float(lat))
            if not eh_numero(cidade.get("x")) or not eh_numero(cidade.get("y")):
                self.problema(etiqueta, "%s: x/y nao numericos" % city_id)
                continue
            if abs(float(cidade["x"]) - esperado_x) > TOLERANCIA_COORD:
                self.problema(etiqueta, "%s: x=%r, esperado %.1f"
                              % (city_id, cidade["x"], esperado_x))
            if abs(float(cidade["y"]) - esperado_y) > TOLERANCIA_COORD:
                self.problema(etiqueta, "%s: y=%r, esperado %.1f"
                              % (city_id, cidade["y"], esperado_y))

    def validar_regiao(self):
        etiqueta = "REGIAO"
        abaixo = []
        for city_id, cidade in sorted(self.cidades_regiao.items()):
            populacao_regiao = cidade.get("population_region")
            urbano = cidade.get("urban_population")
            regiao = cidade.get("region_population")
            if not eh_numero(populacao_regiao) or not eh_numero(urbano) \
                    or not eh_numero(regiao):
                self.problema(etiqueta,
                              "%s: campo de populacao ausente/nao numerico"
                              % city_id)
                continue
            esperado = max(0, populacao_regiao - urbano)
            if not iguais(regiao, esperado):
                self.problema(etiqueta,
                              "%s: region_population=%r != max(0, %r-%r)=%r"
                              % (city_id, regiao, populacao_regiao, urbano,
                                 esperado))
            if populacao_regiao < urbano:
                abaixo.append(city_id)
        self.aberto_urbano = len(abaixo)
        self.resumo.append("cidades com population_region < urban_population: "
                           "%d (esperado %d)"
                           % (len(abaixo), ABAIXO_URBANO_ESPERADAS))
        self.resumo.append("  " + ", ".join(abaixo))
        self.exigir(len(abaixo) == ABAIXO_URBANO_ESPERADAS, etiqueta,
                    "%d cidades com regiao < urbano (esperado %d): %s"
                    % (len(abaixo), ABAIXO_URBANO_ESPERADAS,
                       ", ".join(abaixo)))

    def validar_vizinhos(self):
        etiqueta = "VIZINHOS"
        esperados = {combinado.get("attached_to_before")
                     for combinado in self.patch_estado.values()}
        if set(self.patch_vizinho) != esperados:
            self.problema(etiqueta,
                          "neighbor_patch %s != vizinhos de origem %s"
                          % (sorted(self.patch_vizinho), sorted(esperados)))

        for city_id, patch in sorted(self.patch_vizinho.items()):
            cidade = self.cidades_regiao.get(city_id)
            if cidade is None:
                self.problema(etiqueta,
                              "vizinho %s nao existe em state_regions['cities']"
                              % city_id)
                continue
            if not iguais(cidade.get("population_region", -1),
                          patch.get("population_region_after", -2)):
                self.problema(
                    etiqueta, "%s: population_region=%r != after %r"
                    % (city_id, cidade.get("population_region"),
                       patch.get("population_region_after")))
            if not iguais(cidade.get("region_population", -1),
                          patch.get("region_population_after", -2)):
                self.problema(etiqueta,
                              "%s: region_population=%r != after %r"
                              % (city_id, cidade.get("region_population"),
                                 patch.get("region_population_after")))
            if sorted(cidade.get("states") or []) != sorted(
                    patch.get("states_after") or []):
                self.problema(etiqueta,
                              "%s: states=%r != states_after %r"
                              % (city_id, cidade.get("states"),
                                 patch.get("states_after")))

            removidos = patch.get("removed_states") or []
            if set(removidos) & set(patch.get("states_after") or []):
                self.problema(etiqueta,
                              "%s: states_after ainda contem removidos (%s)"
                              % (city_id,
                                 sorted(set(removidos)
                                        & set(patch.get("states_after") or []))))
            soma_removida = 0
            faltando = []
            for state_id in removidos:
                estado = self.estados.get(state_id)
                if estado is None:
                    faltando.append(state_id)
                    continue
                soma_removida += estado.get("population_2026", 0)
            if faltando:
                self.problema(etiqueta, "%s: removed_states inexistentes: %s"
                              % (city_id, ", ".join(faltando)))
            if not iguais(patch.get("population_removed", -1), soma_removida):
                self.problema(
                    etiqueta, "%s: population_removed=%r != soma %s"
                    % (city_id, patch.get("population_removed"),
                       formatar(soma_removida)))
            if len(removidos) != len(set(removidos)):
                self.problema(etiqueta,
                              "%s: removed_states com repeticao" % city_id)

            for state_id in removidos:
                donos = sorted(
                    outro_id for outro_id, outra
                    in self.cidades_regiao.items()
                    if state_id in (outra.get("states") or [])
                    and outro_id not in set(self.ids_novos))
                if donos:
                    self.problema(etiqueta,
                                  "estado removido %s ainda listado em %s"
                                  % (state_id, ", ".join(donos)))

    def validar_rateio(self):
        etiqueta = "RATEIO"
        elenco_base = [{"id": cidade.get("id"),
                        "nation": cidade.get("nation"),
                        "urban_population": cidade.get("urban_population"),
                        "population": cidade.get("population")}
                       for cidade in self.cidades_json]
        base = distribuir_populacao_nacional(elenco_base,
                                             self.populacao_nacao)
        divergentes = [cidade.get("id") for cidade in self.cidades_json
                       if not iguais(cidade.get("population", -1),
                                     base[cidade.get("id")])]
        if divergentes:
            self.problema(etiqueta,
                          "rateio sobre as 834 nao reproduz cities.json em %d "
                          "cidades (%s)"
                          % (len(divergentes),
                             ", ".join(sorted(divergentes)[:8])))

        elenco_completo = list(elenco_base)
        elenco_completo += [{"id": cidade.get("id"),
                             "nation": cidade.get("nation"),
                             "urban_population": cidade.get("urban_population"),
                             "population": cidade.get("population")}
                            for cidade in self.novas]
        completo = distribuir_populacao_nacional(elenco_completo,
                                                 self.populacao_nacao)

        for cidade in self.novas:
            city_id = cidade.get("id")
            if city_id not in completo:
                self.problema(etiqueta,
                              "%s: fora do elenco do rateio" % city_id)
                continue
            if not iguais(cidade.get("population", -1), completo[city_id]):
                self.problema(etiqueta,
                              "%s: population=%r != rateio %r"
                              % (city_id, cidade.get("population"),
                                 completo[city_id]))

        esperado_mudanca = {}
        for cidade in self.cidades_json:
            city_id = cidade.get("id")
            antes = int(cidade.get("population", 0))
            depois = completo.get(city_id, antes)
            if antes != depois:
                esperado_mudanca[city_id] = {"before": antes, "after": depois}

        informado = self.rateio_extra.get("changed", {}) or {}
        for city_id, valores in sorted(informado.items()):
            if city_id not in esperado_mudanca:
                self.problema(etiqueta,
                              "changed lista %s, que nao muda no rateio"
                              % city_id)
                continue
            esperado = esperado_mudanca[city_id]
            if not eh_inteiro(valores.get("before")) \
                    or not eh_inteiro(valores.get("after")):
                self.problema(etiqueta, "%s: before/after nao sao inteiros"
                              % city_id)
                continue
            if valores["before"] != esperado["before"]:
                self.problema(etiqueta, "%s: before=%r != cities.json %r"
                              % (city_id, valores["before"],
                                 esperado["before"]))
            if valores["after"] != esperado["after"]:
                self.problema(etiqueta, "%s: after=%r != rateio %r"
                              % (city_id, valores["after"],
                                 esperado["after"]))
        ausentes = sorted(set(esperado_mudanca) - set(informado))
        if ausentes:
            self.problema(etiqueta,
                          "changed nao lista %d cidades que mudam (%s)"
                          % (len(ausentes), ", ".join(ausentes[:8])))

        self.resumo.append(
            "rateio: 834 reproduzidas (%d divergentes), 15 novas, %d "
            "alteradas" % (len(divergentes), len(esperado_mudanca)))

    def validar_i18n(self):
        etiqueta = "I18N"
        chaves_por_locale = {}
        for locale in LOCALES:
            tabela = self.i18n_extra.get(locale)
            if not isinstance(tabela, dict):
                self.problema(etiqueta, "extra['i18n']['%s'] ausente" % locale)
                chaves_por_locale[locale] = set()
                continue
            chaves_por_locale[locale] = set(tabela)
            for city_id in self.ids_novos:
                chave = "city.name." + str(city_id)
                valor = tabela.get(chave)
                if not isinstance(valor, str) or not valor.strip():
                    self.problema(etiqueta,
                                  "%s: %s vazio ou ausente" % (locale, chave))

        esperado = {"city.name." + str(city_id) for city_id in self.ids_novos}
        for locale in LOCALES:
            if chaves_por_locale.get(locale, set()) != esperado:
                self.problema(
                    etiqueta, "%s: conjunto de chaves difere dos 15 ids "
                              "(faltam %s ; sobram %s)"
                    % (locale,
                       sorted(esperado - chaves_por_locale.get(locale, set())),
                       sorted(chaves_por_locale.get(locale, set()) - esperado)))

        ja_existem = [city_id for city_id in self.ids_novos
                      if isinstance(self.i18n_json.get("en"), dict)
                      and ("city.name." + str(city_id)) in self.i18n_json["en"]]
        if ja_existem:
            self.resumo.append(
                "aviso: %d chaves novas ja existem em i18n.json (%s)"
                % (len(ja_existem), ", ".join(sorted(ja_existem))))
        else:
            self.resumo.append("i18n: 15/15 chaves nas 3 locales, ainda nao "
                               "aplicadas em i18n.json")

    def validar_contagens(self):
        etiqueta = "COBERTURA"
        self.exigir(len(self.estados) == ESTADOS_ESPERADOS, etiqueta,
                    "%d estados (esperado %d)"
                    % (len(self.estados), ESTADOS_ESPERADOS))
        self.exigir(len(self.cidades_regiao) == CIDADES_ESPERADAS, etiqueta,
                    "%d cidades de regiao (esperado %d)"
                    % (len(self.cidades_regiao), CIDADES_ESPERADAS))
        self.exigir(len(self.cidades_json) == CIDADES_ESPERADAS
                    - CIDADES_NOVAS_ESPERADAS, etiqueta,
                    "%d cidades em cities.json (esperado %d)"
                    % (len(self.cidades_json),
                       CIDADES_ESPERADAS - CIDADES_NOVAS_ESPERADAS))

    def rodar(self):
        self.validar_soma()
        self.validar_estados_novos()
        self.validar_criterio()
        self.validar_ids()
        self.validar_campos()
        self.validar_coordenadas()
        self.validar_regiao()
        self.validar_vizinhos()
        self.validar_rateio()
        self.validar_i18n()
        self.validar_contagens()

    def relatar(self, segundos):
        soma_estados = sum(estado.get("population_2026", 0)
                           for estado in self.estados.values())
        soma_regioes = sum(cidade.get("population_region", 0)
                           for cidade in self.cidades_regiao.values())
        soma_urbana = sum(cidade.get("urban_population", 0)
                          for cidade in self.cidades_regiao.values())

        print("B3 capitais extras - validacao do patch")
        print("estados: %s | cidades de regiao: %s | cidades novas: %d"
              % (formatar(len(self.estados)),
                 formatar(len(self.cidades_regiao)), len(self.novas)))
        print("soma dos estados: %s" % formatar(soma_estados))
        print("soma de population_region: %s" % formatar(soma_regioes))
        print("soma de urban_population: %s" % formatar(soma_urbana))
        print("nacoes que fecham (estado = regiao = nations.json): %d/%d"
              % (self.fecham, self.total_nacoes))
        print("estados >20M sem cidade propria: %d" % self.aberto)
        print("cobertura: %d/%d estados atendidos"
              % (self.cobertura_estados, CIDADES_NOVAS_ESPERADAS))
        print("cidades com regiao abaixo do urbano: %d" % self.aberto_urbano)
        for linha in self.resumo:
            print(linha)

        if self.problemas:
            print("")
            print("PROBLEMAS (%d):" % len(self.problemas))
            for problema in self.problemas:
                print(" - " + problema)
            print("")
            print("RESULTADO: FALHOU - %d problema(s) em %.2f s"
                  % (len(self.problemas), segundos))
            return

        print("")
        print("RESULTADO: OK - 15/15 estados atendidos, 849 cidades, soma %s, "
              "0 problemas (%.2f s)"
              % (formatar(soma_estados), segundos))


def main():
    inicio = time.perf_counter()
    try:
        regioes = carregar(ARQ_REGIOES)
        cidades_json = carregar(ARQ_CIDADES)
        nacoes_json = carregar(ARQ_NACOES)
        extra = carregar(ARQ_EXTRA)
        i18n_json = carregar(ARQ_I18N)
    except (OSError, ValueError) as erro:
        print("RESULTADO: FALHOU - nao foi possivel ler os dados: %s" % erro)
        return 1

    validador = Validador(regioes, cidades_json, nacoes_json, extra, i18n_json)
    validador.rodar()
    validador.relatar(time.perf_counter() - inicio)
    return 0 if not validador.problemas else 1


if __name__ == "__main__":
    sys.exit(main())
