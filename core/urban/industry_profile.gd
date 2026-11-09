extends RefCounted
class_name IndustryProfile

## Perfil industrial derivado da economia real da cidade.
##
## Não existe uma economia paralela: o perfil é apenas uma leitura normalizada
## dos indicadores que já vêm do EconomySim (produção, industrialização, renda,
## estoque). O viés por cidade representa a vocação declarada de cada economia
## (ex.: Londres financeira, Bruxelas siderúrgica) e só redistribui o peso entre
## os setores — nunca cria produção nova.

const SECTORS: Array[String] = ["steel", "manufacturing", "refining", "power", "storage"]

const CITY_BIAS := {
	"GBR-LDN": {"steel": 0.5, "manufacturing": 0.8, "refining": 1.0, "power": 1.2, "storage": 1.5},
	"FRA-PAR": {"steel": 0.7, "manufacturing": 1.3, "refining": 1.0, "power": 1.0, "storage": 0.9},
	"DEU-BER": {"steel": 1.0, "manufacturing": 1.3, "refining": 1.0, "power": 1.1, "storage": 0.9},
	"NED-AMS": {"steel": 0.4, "manufacturing": 0.9, "refining": 1.5, "power": 1.0, "storage": 1.4},
	"BEL-BRU": {"steel": 1.5, "manufacturing": 1.1, "refining": 0.8, "power": 1.0, "storage": 0.9},
}


## Lê os indicadores da cidade e devolve as frações por setor + intensidade.
static func derive(city: Dictionary) -> Dictionary:
	var industrialization := clampf(float(city.get("industrialization", 0.0)), 0.0, 1.0)
	var income := clampf(float(city.get("income", 0.5)), 0.0, 1.0)
	var need := maxf(float(city.get("need", 1.0)), 1.0)
	var storage_ratio := clampf(float(city.get("stockpile", 0.0)) / need, 0.0, 2.0)

	var raw := {
		"steel": industrialization * (1.0 - income) * 1.2,
		"manufacturing": industrialization * (0.6 + 0.4 * income),
		"refining": industrialization * income * 0.9,
		"power": industrialization * 0.6,
		"storage": storage_ratio * 0.3 + industrialization * 0.2,
	}
	return normalize(raw, industrialization, String(city.get("id", "")))


## Normaliza os pesos e aplica o viés da cidade. Sem produção, todos os setores
## ficam zerados e a cidade não recebe distrito industrial.
static func normalize(raw: Dictionary, intensity: float, city_id: String) -> Dictionary:
	var bias: Dictionary = CITY_BIAS.get(city_id, {})
	var total := 0.0
	for sector in SECTORS:
		total += maxf(float(raw.get(sector, 0.0)), 0.0) * float(bias.get(sector, 1.0))

	var profile := {"intensity": clampf(intensity, 0.0, 1.0)}
	for sector in SECTORS:
		profile[sector] = 0.0
	if total <= 0.0:
		return profile

	for sector in SECTORS:
		profile[sector] = maxf(float(raw.get(sector, 0.0)), 0.0) \
				* float(bias.get(sector, 1.0)) / total
	return profile
