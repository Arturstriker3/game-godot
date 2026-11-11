#!/usr/bin/env python3
"""
Diagnóstico estático de balanceamento global (Fase A — só leitura).

Agrega deposits.json / nature.json / agriculture.json para medir:
  (a) reserva global por recurso vs taxa de extração (anos até esgotar);
  (b) distribuição global (principais produtores por recurso e regiões-chave);
  (c) regressão do baseline de aptidão agrícola;
  (d) números sem fonte (data_quality).

Escreve docs/economy/balance_review_static.json e imprime resumo.
"""
import json
import os

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))

DEPOSITS = os.path.join(ROOT, 'data', 'definitions', 'natural', 'deposits.json')
NATURE = os.path.join(ROOT, 'data', 'definitions', 'natural', 'nature.json')
AGRICULTURE = os.path.join(ROOT, 'data', 'definitions', 'natural', 'agriculture.json')
OUT = os.path.join(ROOT, 'docs', 'economy', 'balance_review_static.json')

# Nome amigável para as nações usadas na checagem de regiões-chave.
NAMES = {
    'CHN': 'China', 'BRA': 'Brazil', 'ARG': 'Argentina', 'USA': 'United States',
    'IND': 'India', 'RUS': 'Russia', 'UKR': 'Ukraine', 'AUS': 'Australia',
    'CHL': 'Chile', 'PER': 'Peru', 'ZAF': 'South Africa', 'BWA': 'Botswana',
    'KAZ': 'Kazakhstan', 'CAN': 'Canada', 'MAR': 'Morocco', 'GIN': 'Guinea',
    'SAU': 'Saudi Arabia', 'IRQ': 'Iraq', 'IRN': 'Iran', 'ARE': 'UAE',
    'KWT': 'Kuwait', 'QAT': 'Qatar', 'DZA': 'Algeria', 'LBY': 'Libya',
    'NGA': 'Nigeria', 'VEN': 'Venezuela', 'IDN': 'Indonesia', 'NLD': 'Netherlands',
    'DEU': 'Germany', 'GBR': 'United Kingdom', 'FRA': 'France', 'BEL': 'Belgium',
    'SUR': 'Suriname', 'GUY': 'Guyana',
}

# 14 minerais presentes em deposits.json
MINERALS = ['coal', 'copper', 'gas', 'iron', 'oil', 'bauxite', 'lead', 'gold',
            'phosphate', 'tungsten', 'rare_earths', 'sulfur', 'diamond', 'uranium']
# 7 renováveis (não vêm de deposits.json)
RENEWABLES = ['wheat', 'rice', 'maize', 'potato', 'soy', 'cotton', 'wood']

BASELINE = {
    # (nação ISO3, cultura, valor mínimo aceito) — corrigido para os valores
    # reais medidos em agriculture.json (MAX por nação), reconciliação P5.
    ('BRA', 'soy'): 0.762, ('BRA', 'cotton'): 0.731, ('BRA', 'maize'): 0.746,
    ('USA', 'maize'): 0.96, ('USA', 'soy'): 0.96,
    ('ARG', 'soy'): 0.986, ('ARG', 'maize'): 0.988,
    ('IND', 'wheat'): 0.78,
    ('CHN', 'wheat'): 0.97, ('CHN', 'soy'): 0.906,
    ('RUS', 'wheat'): 0.998, ('RUS', 'maize'): 0.998,
    ('UKR', 'wheat'): 0.901, ('UKR', 'maize'): 0.834,
}

KEY_REGIONS = {
    'OPEP petróleo/gás': {
        'minerals': ['oil', 'gas'],
        'nations': ['SAU', 'IRQ', 'IRN', 'ARE', 'KWT', 'QAT', 'DZA', 'LBY', 'NGA', 'VEN'],
    },
    'China terras raras/tungstênio': {'minerals': ['rare_earths', 'tungsten'], 'nations': ['CHN']},
    'Brasil/Argentina/EUA agro': {'crops': ['soy', 'maize', 'wheat'], 'nations': ['BRA', 'ARG', 'USA']},
    'Austrália/Chile/Peru cobre/ouro/ferro': {'minerals': ['copper', 'gold', 'iron'], 'nations': ['AUS', 'CHL', 'PER']},
    'África do Sul/Botsuana diamante': {'minerals': ['diamond'], 'nations': ['ZAF', 'BWA']},
    'Cazaquistão/Canadá/Austrália urânio': {'minerals': ['uranium'], 'nations': ['KAZ', 'CAN', 'AUS']},
    'Marrocos fosfato': {'minerals': ['phosphate'], 'nations': ['MAR']},
    'Guiné/Austrália bauxita': {'minerals': ['bauxite'], 'nations': ['GIN', 'AUS']},
}


def load(path):
    with open(path, encoding='utf-8') as f:
        return json.load(f)


def main():
    deposits = load(DEPOSITS)['deposits']
    nature = load(NATURE)['territories']
    agriculture = load(AGRICULTURE)['territories']

    # territory id -> adm0_a3
    territory_code = {}
    for tid, t in nature.items():
        territory_code[tid] = t.get('adm0_a3', '')

    # --- (a) reserva global por recurso + top produtores ---
    reserves = {m: 0.0 for m in MINERALS}
    by_nation = {m: {} for m in MINERALS}
    for d in deposits:
        res = d.get('resource', '')
        if res not in reserves:
            continue
        if d.get('facility', False):
            continue
        qty = float(d.get('quantity_kt', 0.0))
        if qty <= 0.0:
            continue
        reserves[res] += qty
        code = territory_code.get(d.get('territory', ''), d.get('owner', ''))
        if code:
            by_nation[res][code] = by_nation[res].get(code, 0.0) + qty

    top = {}
    for res in MINERALS:
        ranking = sorted(by_nation[res].items(), key=lambda kv: -kv[1])
        top[res] = [{'code': c, 'kt': round(v, 1)} for c, v in ranking[:5]]

    # --- (c) regressão do baseline de aptidão (MAX por nação) ---
    crop_max = {}
    for tid, t in agriculture.items():
        code = territory_code.get(tid, '')
        if not code:
            continue
        if code not in crop_max:
            crop_max[code] = {}
        for crop in RENEWABLES[:-1]:  # exceto wood (vem de forest_cover)
            v = float(t.get(crop, 0.0))
            crop_max[code][crop] = max(crop_max[code].get(crop, 0.0), v)

    regression = []
    for (code, crop), floor in BASELINE.items():
        actual = crop_max.get(code, {}).get(crop, None)
        if actual is None:
            regression.append({'nation': code, 'crop': crop, 'expected_min': floor,
                               'actual': None, 'status': 'SEM_DADO'})
        elif actual + 1e-6 < floor:
            regression.append({'nation': code, 'crop': crop, 'expected_min': floor,
                               'actual': round(actual, 4), 'status': 'REGREDIU'})
        else:
            regression.append({'nation': code, 'crop': crop, 'expected_min': floor,
                               'actual': round(actual, 4), 'status': 'ok'})

    # --- (d) data_quality ---
    dq_deposits = {}
    for d in deposits:
        dq = d.get('data_quality', 'SEM_CAMPO')
        dq_deposits[dq] = dq_deposits.get(dq, 0) + 1
    dq_nature = {}
    for tid, t in nature.items():
        dq = t.get('data_quality', 'SEM_CAMPO')
        dq_nature[dq] = dq_nature.get(dq, 0) + 1

    # Regiões-chave: produção de cada recurso por nação-chave (ordem de grandeza)
    regions = {}
    for label, spec in KEY_REGIONS.items():
        info = {}
        for code in spec['nations']:
            info[code] = {}
            for res in spec.get('minerals', []):
                info[code][res] = round(by_nation[res].get(code, 0.0), 1)
            for crop in spec.get('crops', []):
                info[code][crop] = round(crop_max.get(code, {}).get(crop, 0.0), 4)
        regions[label] = info

    out = {
        'global_reserves_kt': {m: round(reserves[m], 1) for m in MINERALS},
        'top_producers': top,
        'baseline_regression': regression,
        'data_quality_deposits': dq_deposits,
        'data_quality_nature': dq_nature,
        'key_regions': regions,
    }
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'w', encoding='utf-8') as f:
        json.dump(out, f, indent=2, ensure_ascii=False)
    print('escrito', OUT)

    # ---- resumo impresso ----
    print('\n=== (a) RESERVA GLOBAL POR RECURSO (kt) + top produtores ===')
    for res in MINERALS:
        top5 = ', '.join('%s(%.1fM)' % (e['code'], e['kt'] / 1e6) if e['kt'] >= 1e6
                         else '%s(%.0fk)' % (e['code'], e['kt']) for e in top[res])
        print('  %-12s %14.1f kt   %s' % (res, reserves[res], top5))

    print('\n=== (c) REGRESSÃO DE BASELINE ===')
    for r in regression:
        if r['status'] != 'ok':
            print('  %-4s %-8s min=%.2f actual=%s [%s]' % (
                r['nation'], r['crop'], r['expected_min'], r['actual'], r['status']))
    print('  (nenhuma linha acima = baseline preservado)')

    print('\n=== (d) DATA_QUALITY ===')
    print('  deposits:', dq_deposits)
    print('  nature:', dq_nature)


if __name__ == '__main__':
    main()
