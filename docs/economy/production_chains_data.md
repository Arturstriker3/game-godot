# Cadeias de produção — dados derivados (F1)

Gerado por `tools/build_goods_chain.py report`. Não editar à mão.

- Bens: **115** (21 primários em `order` 0..20, 94 novos em 21..114)
- Receitas: **114** (coeficiente unitário padrão onde a spec não traz qty)
- Preços: primários = `calibrated:legacy` (dado já calibrado); novos = `derived:v1`
  pela fórmula `min_receita(Σ qty × preço_insumo / yield) × (1 + margem)`.
- Margens: raw 0 · byproduct 0,10 · intermediate 0,25 · final 0,45 · durável/luxo 0,80.
- Subprodutos sem receita própria: 0,10 × preço do bem que os gera.
- `charcoal` é alias de `coal` (a spec diz que produz o MESMO item).
- Parâmetros de fábrica (`jobs_per_ton`, `wage`, `opex_per_ton`, `capital_per_capacity`) são **estimativa** de ordem de grandeza até o laboratório de F2.

## Preços derivados

| # | bem | tier | kind | setor | preço | procedência |
|---:|---|---|---|---|---:|---|
| 0 | `wheat` | raw | raw | food | 40.00 | `calibrated:legacy` |
| 1 | `iron` | raw | raw | mining | 60.00 | `calibrated:legacy` |
| 2 | `rice` | raw | raw | food | 55.00 | `calibrated:legacy` |
| 3 | `maize` | raw | raw | food | 42.00 | `calibrated:legacy` |
| 4 | `bauxite` | raw | raw | mining | 15.00 | `calibrated:legacy` |
| 5 | `lead` | raw | raw | mining | 25.00 | `calibrated:legacy` |
| 6 | `gold` | raw | raw | mining | 3000.00 | `calibrated:legacy` |
| 7 | `phosphate` | raw | raw | mining | 10.00 | `calibrated:legacy` |
| 8 | `tungsten` | raw | raw | mining | 200.00 | `calibrated:legacy` |
| 9 | `rare_earths` | raw | raw | mining | 400.00 | `calibrated:legacy` |
| 10 | `sulfur` | raw | raw | mining | 15.00 | `calibrated:legacy` |
| 11 | `diamond` | raw | raw | mining | 5000.00 | `calibrated:legacy` |
| 12 | `coal` | raw | raw | energy | 6.00 | `calibrated:legacy` |
| 13 | `copper` | raw | raw | mining | 60.00 | `calibrated:legacy` |
| 14 | `gas` | raw | raw | energy | 30.00 | `calibrated:legacy` |
| 15 | `oil` | raw | raw | energy | 45.00 | `calibrated:legacy` |
| 16 | `potato` | raw | raw | food | 35.00 | `calibrated:legacy` |
| 17 | `soy` | raw | raw | food | 45.00 | `calibrated:legacy` |
| 18 | `cotton` | raw | raw | textile | 70.00 | `calibrated:legacy` |
| 19 | `wood` | raw | raw | wood | 25.00 | `calibrated:legacy` |
| 20 | `uranium` | raw | raw | energy | 500.00 | `calibrated:legacy` |
| 21 | `flour` | semi | intermediate | food | 50.00 | `derived:v1` |
| 22 | `bran` | semi | byproduct | food | 44.00 | `derived:v1` |
| 23 | `bread` | final | final | food | 72.50 | `derived:v1` |
| 24 | `bagged_rice` | final | final | food | 145.45 | `derived:v1` |
| 25 | `feed` | semi | intermediate | food | 52.50 | `derived:v1` |
| 26 | `meat` | final | final | food | 76.12 | `derived:v1` |
| 27 | `canned_food` | final | final | food | 144.36 | `derived:v1` |
| 28 | `combat_ration` | final | final | food | 401.26 | `derived:v1` |
| 29 | `alcohol` | final | final | food | 84.74 | `derived:v1` |
| 30 | `premium_spirits` | final | final | food | 168.19 | `derived:v1` |
| 31 | `ethanol` | semi | intermediate | food | 52.50 | `derived:v1` |
| 32 | `fertilizer` | final | final | agro | 58.00 | `derived:v1` |
| 33 | `organic_fertilizer` | final | final | agro | 212.67 | `derived:v1` |
| 34 | `fabric` | semi | intermediate | textile | 87.50 | `derived:v1` |
| 35 | `synthetic_fabric` | semi | intermediate | textile | 117.19 | `derived:v1` |
| 36 | `clothes` | final | final | textile | 126.88 | `derived:v1` |
| 37 | `uniforms` | final | final | textile | 126.88 | `derived:v1` |
| 38 | `shoes` | final | final | textile | 208.44 | `derived:v1` |
| 39 | `planks` | semi | intermediate | wood | 31.25 | `derived:v1` |
| 40 | `sawdust` | semi | byproduct | wood | 27.50 | `derived:v1` |
| 41 | `furniture` | final | final | wood | 56.25 | `derived:v1` |
| 42 | `luxury_furniture` | final | final | wood | 213.75 | `derived:v1` |
| 43 | `cellulose` | semi | intermediate | wood | 31.25 | `derived:v1` |
| 44 | `paper` | final | final | wood | 45.31 | `derived:v1` |
| 45 | `pallets` | final | final | wood | 45.31 | `derived:v1` |
| 46 | `charcoal` | semi | intermediate | wood | 6.00 | `alias:coal` |
| 47 | `gasoline` | final | final | chemical | 78.61 | `derived:co_product:v1` |
| 48 | `plastic` | semi | intermediate | chemical | 56.25 | `derived:v1` |
| 49 | `chemicals` | semi | intermediate | chemical | 37.50 | `derived:v1` |
| 50 | `rubber` | semi | intermediate | chemical | 79.69 | `derived:v1` |
| 51 | `refined_sulfur` | semi | intermediate | chemical | 18.75 | `derived:v1` |
| 52 | `gunpowder` | semi | intermediate | chemical | 30.94 | `derived:v1` |
| 53 | `explosives` | semi | intermediate | chemical | 85.55 | `derived:v1` |
| 54 | `carbon_fiber` | semi | intermediate | chemical | 124.69 | `derived:v1` |
| 55 | `medicine` | final | final | chemical | 135.94 | `derived:v1` |
| 56 | `antibiotics` | final | final | chemical | 54.38 | `derived:v1` |
| 57 | `vaccines` | final | final | chemical | 251.49 | `derived:v1` |
| 58 | `plastic_parts` | semi | intermediate | chemical | 70.31 | `derived:v1` |
| 59 | `iron_ingot` | semi | intermediate | metal | 75.00 | `derived:v1` |
| 60 | `steel` | semi | intermediate | metal | 101.25 | `derived:v1` |
| 61 | `steel_sheet` | semi | intermediate | metal | 126.56 | `derived:v1` |
| 62 | `special_alloy` | semi | intermediate | metal | 439.06 | `derived:v1` |
| 63 | `cast_parts` | semi | intermediate | metal | 101.25 | `derived:v1` |
| 64 | `mechanical_parts` | semi | intermediate | metal | 93.75 | `derived:v1` |
| 65 | `hardware` | semi | intermediate | metal | 93.75 | `derived:v1` |
| 66 | `copper_ingot` | semi | intermediate | metal | 75.00 | `derived:v1` |
| 67 | `wires` | semi | intermediate | metal | 33.48 | `derived:v1` |
| 68 | `aluminum_ingot` | semi | intermediate | metal | 18.75 | `derived:v1` |
| 69 | `lead_ingot` | semi | intermediate | metal | 31.25 | `derived:v1` |
| 70 | `tungsten_ingot` | semi | intermediate | metal | 250.00 | `derived:v1` |
| 71 | `gold_bar` | semi | intermediate | metal | 3750.00 | `derived:v1` |
| 72 | `processed_rare_earths` | semi | intermediate | metal | 500.00 | `derived:v1` |
| 73 | `tools` | final | final | machinery | 154.06 | `derived:v1` |
| 74 | `industrial_tools` | semi | intermediate | machinery | 439.06 | `derived:v1` |
| 75 | `precision_tools` | semi | intermediate | machinery | 6798.83 | `derived:v1` |
| 76 | `engine` | semi | intermediate | machinery | 257.53 | `derived:v1` |
| 77 | `micromotor` | semi | intermediate | machinery | 666.85 | `derived:v1` |
| 78 | `tires` | semi | intermediate | machinery | 246.10 | `derived:v1` |
| 79 | `chassis` | semi | intermediate | machinery | 275.39 | `derived:v1` |
| 80 | `tractor` | final | final | machinery | 1402.24 | `derived:v1` |
| 81 | `mining_machine` | final | final | machinery | 1436.11 | `derived:v1` |
| 82 | `robots` | final | final | machinery | 75313.17 | `derived:v1` |
| 83 | `industrial_machinery` | final | final | machinery | 13459.54 | `derived:v1` |
| 84 | `spare_parts` | final | final | machinery | 492.78 | `derived:v1` |
| 85 | `construction_material` | final | final | construction | 181.25 | `derived:v1` |
| 86 | `steel_structure` | semi | intermediate | construction | 243.75 | `derived:v1` |
| 87 | `electrical_material` | semi | intermediate | construction | 6176.61 | `derived:v1` |
| 88 | `truck` | final | final | transport | 1556.93 | `derived:v1` |
| 89 | `car` | final | final | transport | 1911.29 | `derived:v1` |
| 90 | `naval_hull` | semi | intermediate | transport | 431.25 | `derived:v1` |
| 91 | `cargo_ship` | final | final | transport | 56493.95 | `derived:v1` |
| 92 | `fuselage` | semi | intermediate | transport | 728.12 | `derived:v1` |
| 93 | `cargo_plane` | final | final | transport | 57028.32 | `derived:v1` |
| 94 | `electronic_components` | semi | intermediate | electronics | 4851.56 | `derived:v1` |
| 95 | `circuit_board` | semi | intermediate | electronics | 6228.51 | `derived:v1` |
| 96 | `chips` | semi | intermediate | electronics | 15234.86 | `derived:v1` |
| 97 | `sensors` | semi | intermediate | electronics | 19668.58 | `derived:v1` |
| 98 | `batteries` | semi | intermediate | electronics | 85.94 | `derived:v1` |
| 99 | `appliances` | final | final | electronics | 10287.50 | `derived:v1` |
| 100 | `computer` | final | final | electronics | 38760.62 | `derived:v1` |
| 101 | `phone` | final | final | electronics | 38915.32 | `derived:v1` |
| 102 | `drone` | final | final | electronics | 64307.77 | `derived:v1` |
| 103 | `containers` | semi | intermediate | packaging | 23.44 | `derived:v1` |
| 104 | `jewelry` | final | final | luxury | 15750.00 | `derived:v1` |
| 105 | `casing` | semi | intermediate | military | 93.75 | `derived:v1` |
| 106 | `projectile` | semi | intermediate | military | 39.06 | `derived:v1` |
| 107 | `ammunition` | final | final | military | 237.44 | `derived:v1` |
| 108 | `artillery_ammo` | final | final | military | 406.80 | `derived:v1` |
| 109 | `small_arms` | final | final | military | 351.00 | `derived:v1` |
| 110 | `artillery` | final | final | military | 37348.56 | `derived:v1` |
| 111 | `military_drone` | final | final | military | 93771.35 | `derived:v1` |
| 112 | `infantry_gear` | final | final | military | 7422.19 | `derived:v1` |
| 113 | `diesel` | final | final | chemical | 58.00 | `derived:v1` |
| 114 | `jet_fuel` | final | final | chemical | 78.61 | `derived:co_product:v1` |

## Receitas normalizadas

- `flour` â†’ `flour` (yield 1): 1× `wheat` + subproduto 1× `bran`
- `bread` â†’ `bread` (yield 1): 1× `flour`
- `bagged_rice_a` â†’ `bagged_rice` (yield 1): 1× `rice`, 1× `plastic`
- `bagged_rice_b` â†’ `bagged_rice` (yield 1): 1× `rice`, 1× `paper`
- `feed_a` â†’ `feed` (yield 1): 1× `maize`
- `feed_b` â†’ `feed` (yield 1): 1× `soy`
- `feed_c` â†’ `feed` (yield 0.8): 1× `bran`
- `feed_d` â†’ `feed` (yield 0.7): 1× `rice`
- `feed_e` â†’ `feed` (yield 0.5): 1× `potato`
- `feed_f` â†’ `feed` (yield 0.6): 1× `wheat`
- `meat` â†’ `meat` (yield 1): 1× `feed`
- `canned_food` â†’ `canned_food` (yield 1): 1× `meat`, 1× `containers`
- `combat_ration` â†’ `combat_ration` (yield 1): 1× `canned_food`, 1× `meat`, 1× `plastic`
- `alcohol_a` â†’ `alcohol` (yield 1): 1× `wheat`, 1× `containers`
- `alcohol_b` â†’ `alcohol` (yield 1): 1× `rice`, 1× `containers`
- `alcohol_c` â†’ `alcohol` (yield 1): 1× `maize`, 1× `containers`
- `alcohol_d` â†’ `alcohol` (yield 1): 1× `potato`, 1× `containers`
- `premium_spirits` â†’ `premium_spirits` (yield 1): 1× `alcohol`, 1× `planks`
- `ethanol` â†’ `ethanol` (yield 1): 1× `maize`
- `fertilizer` â†’ `fertilizer` (yield 1): 1× `gas`, 1× `phosphate`
- `organic_fertilizer` â†’ `organic_fertilizer` (yield 0.3): 1× `bran`
- `fabric` â†’ `fabric` (yield 1): 1× `cotton`
- `synthetic_fabric` â†’ `synthetic_fabric` (yield 1): 1× `plastic`, 1× `chemicals`
- `clothes_a` â†’ `clothes` (yield 1): 1× `fabric`
- `clothes_b` â†’ `clothes` (yield 1): 1× `synthetic_fabric`
- `uniforms_a` â†’ `uniforms` (yield 1): 1× `fabric`
- `uniforms_b` â†’ `uniforms` (yield 1): 1× `synthetic_fabric`
- `shoes` â†’ `shoes` (yield 1): 1× `fabric`, 1× `plastic`
- `planks` â†’ `planks` (yield 1): 1× `wood` + subproduto 1× `sawdust`
- `furniture` â†’ `furniture` (yield 1): 1× `planks`
- `luxury_furniture` â†’ `luxury_furniture` (yield 1): 1× `planks`, 1× `fabric`
- `cellulose_a` â†’ `cellulose` (yield 1): 1× `wood`
- `cellulose_b` â†’ `cellulose` (yield 0.7): 1× `sawdust`
- `paper` â†’ `paper` (yield 1): 1× `cellulose`
- `pallets_a` â†’ `pallets` (yield 1): 1× `planks`
- `pallets_b` â†’ `pallets` (yield 0.7): 1× `sawdust`
- `charcoal` â†’ `coal` (yield 1): 1× `wood`
- `gasoline_a` â†’ `gasoline` (yield 0.45): 1× `oil` + subproduto 0.29× `diesel`, 0.09× `jet_fuel`
- `gasoline_b` â†’ `gasoline` (yield 0.8): 1× `ethanol`
- `plastic_a` â†’ `plastic` (yield 1): 1× `oil`
- `plastic_b` â†’ `plastic` (yield 0.5): 1× `gas`
- `chemicals_a` â†’ `chemicals` (yield 1): 1× `oil`, 1× `refined_sulfur`
- `chemicals_b` â†’ `chemicals` (yield 0.5): 1× `gas`
- `chemicals_c` â†’ `chemicals` (yield 0.2): 1× `coal`
- `rubber` â†’ `rubber` (yield 1): 1× `oil`, 1× `refined_sulfur`
- `refined_sulfur` â†’ `refined_sulfur` (yield 1): 1× `sulfur`
- `gunpowder` â†’ `gunpowder` (yield 1): 1× `refined_sulfur`, 1× `coal`
- `explosives` â†’ `explosives` (yield 1): 1× `gunpowder`, 1× `chemicals`
- `carbon_fiber` â†’ `carbon_fiber` (yield 1): 1× `plastic`, 1× `chemicals`, 1× `coal`
- `medicine` â†’ `medicine` (yield 1): 1× `chemicals`, 1× `plastic`
- `antibiotics` â†’ `antibiotics` (yield 1): 1× `chemicals`
- `vaccines` â†’ `vaccines` (yield 1): 1× `medicine`, 1× `chemicals`
- `plastic_parts` â†’ `plastic_parts` (yield 1): 1× `plastic`
- `iron_ingot` â†’ `iron_ingot` (yield 1): 1× `iron`
- `steel` â†’ `steel` (yield 1): 1× `iron_ingot`, 1× `coal`
- `steel_sheet` â†’ `steel_sheet` (yield 1): 1× `steel`
- `special_alloy` â†’ `special_alloy` (yield 1): 1× `steel`, 1× `tungsten_ingot`
- `cast_parts` â†’ `cast_parts` (yield 1): 1× `iron_ingot`, 1× `coal`
- `mechanical_parts` â†’ `mechanical_parts` (yield 1): 1× `iron_ingot`
- `hardware` â†’ `hardware` (yield 1): 1× `iron_ingot`
- `copper_ingot` â†’ `copper_ingot` (yield 1): 1× `copper`
- `wires_a` â†’ `wires` (yield 1): 1× `copper_ingot`
- `wires_b` â†’ `wires` (yield 0.7): 1× `aluminum_ingot`
- `aluminum_ingot` â†’ `aluminum_ingot` (yield 1): 1× `bauxite`
- `lead_ingot` â†’ `lead_ingot` (yield 1): 1× `lead`
- `tungsten_ingot` â†’ `tungsten_ingot` (yield 1): 1× `tungsten`
- `gold_bar` â†’ `gold_bar` (yield 1): 1× `gold`
- `processed_rare_earths` â†’ `processed_rare_earths` (yield 1): 1× `rare_earths`
- `tools` â†’ `tools` (yield 1): 1× `planks`, 1× `iron_ingot`
- `industrial_tools` â†’ `industrial_tools` (yield 1): 1× `steel`, 1× `tungsten_ingot`
- `precision_tools` â†’ `precision_tools` (yield 1): 1× `industrial_tools`, 1× `diamond`
- `engine_a` â†’ `engine` (yield 1): 1× `steel`, 1× `mechanical_parts`, 1× `wires`, 1× `cast_parts`
- `engine_b` â†’ `engine` (yield 1.2): 1× `steel`, 1× `mechanical_parts`, 1× `wires`, 1× `aluminum_ingot`
- `micromotor` â†’ `micromotor` (yield 1): 1× `wires`, 1× `processed_rare_earths`
- `tires` â†’ `tires` (yield 1): 1× `rubber`, 1× `synthetic_fabric`
- `chassis` â†’ `chassis` (yield 1): 1× `steel_sheet`, 1× `mechanical_parts`
- `tractor` â†’ `tractor` (yield 1): 1× `chassis`, 1× `engine`, 1× `tires`
- `mining_machine` â†’ `mining_machine` (yield 1): 1× `steel`, 1× `engine`, 1× `industrial_tools`
- `robots` â†’ `robots` (yield 1): 1× `mechanical_parts`, 1× `chips`, 1× `micromotor`, 1× `sensors`, 1× `electrical_material`
- `industrial_machinery` â†’ `industrial_machinery` (yield 1): 1× `mechanical_parts`, 1× `industrial_tools`, 1× `micromotor`, 1× `cast_parts`, 1× `electrical_material`
- `spare_parts` â†’ `spare_parts` (yield 1): 1× `mechanical_parts`, 1× `tires`
- `construction_material` â†’ `construction_material` (yield 1): 1× `planks`, 1× `hardware`
- `steel_structure` â†’ `steel_structure` (yield 1): 1× `steel`, 1× `hardware`
- `electrical_material` â†’ `electrical_material` (yield 1): 1× `wires`, 1× `plastic`, 1× `electronic_components`
- `truck` â†’ `truck` (yield 1): 1× `chassis`, 1× `engine`, 1× `tires`, 1× `batteries`
- `car` â†’ `car` (yield 1): 1× `chassis`, 1× `steel_sheet`, 1× `engine`, 1× `tires`, 1× `plastic_parts`, 1× `batteries`
- `naval_hull` â†’ `naval_hull` (yield 1): 1× `steel_structure`, 1× `steel`
- `cargo_ship` â†’ `cargo_ship` (yield 1): 1× `naval_hull`, 1× `engine`, 1× `electronic_components`, 1× `electrical_material`, 1× `sensors`
- `fuselage` â†’ `fuselage` (yield 1): 1× `aluminum_ingot`, 1× `special_alloy`, 1× `carbon_fiber`
- `cargo_plane` â†’ `cargo_plane` (yield 1): 1× `fuselage`, 1× `engine`, 1× `electronic_components`, 1× `electrical_material`, 1× `sensors`
- `electronic_components` â†’ `electronic_components` (yield 1): 1× `copper_ingot`, 1× `plastic`, 1× `gold_bar`
- `circuit_board` â†’ `circuit_board` (yield 1): 1× `electronic_components`, 1× `copper_ingot`, 1× `plastic`
- `chips` â†’ `chips` (yield 1): 1× `electronic_components`, 1× `processed_rare_earths`, 1× `precision_tools`, 1× `chemicals`
- `sensors` â†’ `sensors` (yield 1): 1× `chips`, 1× `processed_rare_earths`
- `batteries` â†’ `batteries` (yield 1): 1× `lead_ingot`, 1× `chemicals`
- `appliances` â†’ `appliances` (yield 1): 1× `steel_sheet`, 1× `micromotor`, 1× `electronic_components`, 1× `plastic_parts`
- `computer` â†’ `computer` (yield 1): 1× `circuit_board`, 1× `chips`, 1× `plastic_parts`
- `phone` â†’ `phone` (yield 1): 1× `circuit_board`, 1× `chips`, 1× `plastic_parts`, 1× `batteries`
- `drone` â†’ `drone` (yield 1): 1× `plastic_parts`, 1× `micromotor`, 1× `chips`, 1× `sensors`, 1× `batteries`
- `containers_a` â†’ `containers` (yield 1): 1× `aluminum_ingot`
- `containers_b` â†’ `containers` (yield 1): 1× `plastic`
- `containers_c` â†’ `containers` (yield 1): 1× `planks`
- `containers_d` â†’ `containers` (yield 1): 1× `paper`
- `jewelry` â†’ `jewelry` (yield 1): 1× `gold_bar`, 1× `diamond`
- `casing` â†’ `casing` (yield 1): 1× `copper_ingot`
- `projectile` â†’ `projectile` (yield 1): 1× `lead_ingot`
- `ammunition` â†’ `ammunition` (yield 1): 1× `casing`, 1× `projectile`, 1× `gunpowder`
- `artillery_ammo` â†’ `artillery_ammo` (yield 1): 1× `casing`, 1× `explosives`, 1× `steel`
- `small_arms` â†’ `small_arms` (yield 1): 1× `steel`, 1× `hardware`
- `artillery` â†’ `artillery` (yield 1): 1× `steel`, 1× `special_alloy`, 1× `industrial_tools`, 1× `cast_parts`, 1× `sensors`
- `military_drone` â†’ `military_drone` (yield 1): 1× `drone`, 1× `ammunition`, 1× `carbon_fiber`
- `infantry_gear` â†’ `infantry_gear` (yield 1): 1× `fabric`, 1× `hardware`, 1× `electronic_components`, 1× `batteries`
- `diesel_a` â†’ `diesel` (yield 0.3): 1× `gas`
- `diesel_b` â†’ `diesel` (yield 0.15): 1× `coal`
