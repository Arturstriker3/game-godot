#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""Replica em Python da bacia provincia -> cidade de `core/economy/geography_service.gd`.

Espelha `assign_provinces` MAIS `_share_starved_provinces` (o que
`tools/audit_province_areas.py` fazia so para o passe principal):

1. cada provincia do atlas admin-1 vai para a cidade da MESMA nacao mais
   proxima do centroide da provincia, restrita a MESMA massa de terra
   (`_nearest_city_of_nation`); sem candidata na massa de terra, cai para a
   cidade mais proxima da nacao em qualquer massa;
2. cidade jogavel que ficou sem terra recebe a fracao da provincia da nacao
   mais proxima do seu centro, tomada do dono na razao inversa das distancias
   (`city_share = d_dono / (d_cidade + d_dono)`, no maximo metade do que o dono
   ainda tem); pais sem provincia no atlas (Vaticano) recebe o piso
   `CITY_STATE_MIN_SHARE` (0,1%) de uma provincia qualquer.

Diferencas DELIBERADAS de forma (o resultado e o mesmo):

- as distancias sao comparadas ao quadrado (ordem identica a `Vector2.distance_to`);
- o empate fica com o primeiro candidato, como o `<` estrito do GDScript
  (`numpy.argmin` tambem devolve a primeira ocorrencia);
- a massa de terra de cada provincia e pre-calculada de uma vez em vez de
  recalculada a cada consulta (mesmo resultado, ~O(n) em vez de O(n*m)).

Uso (pela simulacao offline; o jogo continua usando o GDScript):

    from geography_replica import assign_provinces
    catchment, share = assign_provinces(cities, provinces, playable_tags)
"""
import numpy as np

## Mesma constante de `GeographyService`.
PROVINCE_TAG_OVERRIDES = {"XKX": "KOS", "SSD": "SDS"}
## Mesmo piso de `GeographyService.CITY_STATE_MIN_SHARE`.
CITY_STATE_MIN_SHARE = 0.001


def province_tag_map(playable_tags):
    """tag do jogo -> tag `nation` do atlas admin-1 (ex.: XKX -> KOS)."""
    return {tag: PROVINCE_TAG_OVERRIDES.get(tag, tag) for tag in playable_tags}


def game_tag_map(playable_tags):
    """tag `nation` do atlas -> tag do jogo (inverso de `province_tag_map`)."""
    return {PROVINCE_TAG_OVERRIDES.get(tag, tag): tag for tag in playable_tags}


def normalize_cities(cities):
    """{"id","tag"/"nation","x","y","landmass"} -> lista normalizada."""
    out = []
    for city in cities:
        out.append({
            "id": str(city["id"]),
            "tag": str(city.get("nation", city.get("tag", ""))),
            "x": float(city["x"]),
            "y": float(city["y"]),
            "landmass": str(city.get("landmass") or ""),
        })
    return out


def _dist2(points, city_xy):
    """Matriz (pontos x cidades) de distancia ao quadrado, no plano do jogo."""
    dx = points[:, None, 0] - city_xy[None, :, 0]
    dy = points[:, None, 1] - city_xy[None, :, 1]
    return dx * dx + dy * dy


def landmass_of(points, city_xy, city_landmass, chunk=1024):
    """Massa de terra da cidade mais proxima de cada ponto (`_landmass_of`)."""
    points = np.asarray(points, dtype=np.float64)
    out = np.empty(len(points), dtype=object)
    for start in range(0, len(points), chunk):
        block = points[start:start + chunk]
        out[start:start + chunk] = city_landmass[_dist2(block, city_xy).argmin(axis=1)]
    return out


def assign_provinces(cities, provinces, playable_tags):
    """Replica `GeographyService.assign_provinces`.

    Devolve `(catchment, share)`:
      catchment: {city_id: [province_id, ...]}
      share:     {city_id: {province_id: fracao}}  (ausente = 1.0, como no GD)
    """
    norm = normalize_cities(cities)
    if not norm or not provinces:
        return {}, {}
    city_xy = np.array([[c["x"], c["y"]] for c in norm], dtype=np.float64)
    city_lm = np.array([c["landmass"] for c in norm], dtype=object)
    by_tag = {}
    for index, city in enumerate(norm):
        by_tag.setdefault(city["tag"], []).append(index)

    prov_xy = np.array([[float(p["x"]), float(p["y"])] for p in provinces],
                       dtype=np.float64)
    prov_id = [int(p["id"]) for p in provinces]
    prov_nation = np.array([str(p.get("nation") or "") for p in provinces], dtype=object)
    prov_lm = landmass_of(prov_xy, city_xy, city_lm)

    tag_of = province_tag_map(playable_tags)
    playable = set(playable_tags)
    catchment = {}
    share = {}

    def nearest_of(candidates, point):
        """Indice do candidato mais proximo (primeiro no empate)."""
        d = _dist2(point.reshape(1, 2), city_xy[candidates])[0]
        return candidates[int(np.argmin(d))] if candidates else None

    # --- passe principal: provincia -> cidade da nacao -----------------------
    for game_tag in sorted(playable):
        atlas_tag = tag_of[game_tag]
        province_rows = np.flatnonzero(prov_nation == atlas_tag)
        candidates = by_tag.get(game_tag)
        if province_rows.size == 0 or not candidates:
            continue
        candidates = np.asarray(candidates, dtype=np.int64)
        cand_lm = city_lm[candidates]
        dist = _dist2(prov_xy[province_rows], city_xy[candidates])
        for row, province_index in enumerate(province_rows):
            landmass = prov_lm[province_index]
            if landmass == "":
                same = np.ones(len(candidates), dtype=bool)
            else:
                same = cand_lm == landmass
            if same.any():
                masked = np.where(same, dist[row], np.inf)
                owner = candidates[int(np.argmin(masked))]
            else:
                owner = candidates[int(np.argmin(dist[row]))]
            city_id = norm[owner]["id"]
            catchment.setdefault(city_id, []).append(prov_id[province_index])

    # --- cidades sem terra: fracao da provincia mais proxima ----------------
    game_of = game_tag_map(playable)

    def nearest_province_of_tag(atlas_tag, center, landmass):
        mask = prov_nation == atlas_tag
        if landmass != "":
            mask &= prov_lm == landmass
        rows = np.flatnonzero(mask)
        if rows.size == 0:
            return None
        d = _dist2(center.reshape(1, 2), prov_xy[rows])[0]
        return int(rows[int(np.argmin(d))])

    def nearest_any_province(center, landmass):
        mask = np.ones(len(prov_id), dtype=bool)
        if landmass != "":
            mask &= prov_lm == landmass
        rows = np.flatnonzero(mask)
        if rows.size == 0:
            return None
        d = _dist2(center.reshape(1, 2), prov_xy[rows])[0]
        return int(rows[int(np.argmin(d))])

    by_nation = {}
    for index, city in enumerate(norm):
        if city["tag"] in playable:
            by_nation.setdefault(city["tag"], []).append(index)

    for tag, peers in by_nation.items():  # ordem de primeira aparicao
        for city_index in peers:
            city = norm[city_index]
            city_id = city["id"]
            if catchment.get(city_id):
                continue
            center = np.array([city["x"], city["y"]], dtype=np.float64)
            landmass = city["landmass"]
            atlas_tag = tag_of[tag]
            row = nearest_province_of_tag(atlas_tag, center, landmass)
            if row is None:
                row = nearest_province_of_tag(atlas_tag, center, "")
            city_state = row is None
            if row is None:
                row = nearest_any_province(center, landmass)
            if row is None:
                row = nearest_any_province(center, "")
            if row is None:
                continue
            pid = prov_id[row]
            point = prov_xy[row]
            owner_tag = game_of.get(str(prov_nation[row]), "")
            if owner_tag == "":
                owner_id = city_id
            else:
                owner = nearest_of(by_tag.get(owner_tag, []), point)
                owner_id = norm[owner]["id"] if owner is not None else ""
            if owner_id == city_id:
                continue
            if owner_id == "":
                share.setdefault(city_id, {})[pid] = CITY_STATE_MIN_SHARE
                catchment.setdefault(city_id, []).append(pid)
                continue
            owner_center = np.array([0.0, 0.0])
            for peer in norm:
                if peer["id"] == owner_id:
                    owner_center = np.array([peer["x"], peer["y"]], dtype=np.float64)
                    break
            owner_distance = float(np.hypot(*(point - owner_center)))
            city_distance = float(np.hypot(*(point - center)))
            total = city_distance + owner_distance
            city_share = owner_distance / total if total > 0.0 else 0.5
            if city_state:
                city_share = max(city_share, CITY_STATE_MIN_SHARE)
            owner_share = float(share.get(owner_id, {}).get(pid, 1.0))
            city_share = min(city_share, owner_share * 0.5)
            if city_share <= 0.0:
                continue
            share.setdefault(owner_id, {})[pid] = owner_share - city_share
            share.setdefault(city_id, {})[pid] = city_share
            catchment.setdefault(city_id, []).append(pid)
    return catchment, share


def catchment_population(catchment, share, population_by_province, city_id):
    """Populacao da regiao de uma cidade: soma das provincias x fracao."""
    total = 0.0
    for pid in catchment.get(city_id, []):
        fraction = float(share.get(city_id, {}).get(pid, 1.0))
        total += float(population_by_province.get(pid, 0.0)) * fraction
    return total
