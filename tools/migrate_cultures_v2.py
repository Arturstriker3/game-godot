#!/usr/bin/env python3
"""Migra a curadoria de culturas para o modelo v2 (uma cultura nacional por nação).

Lê ``tools/culture_sources/cultures.csv``, ``nation_cultures.csv`` e
``city_overrides_*.csv`` (modelo v1.2, com subgrupos, ``parent`` e identidades
hifenizadas) e grava, nos mesmos arquivos, o modelo v2:

* exatamente uma cultura nacional por nação do cenário, com gentílico do país
  (``nation`` = tag);
* povos sem estado como culturas próprias (``nation`` vazio e ``home_region``);
* nenhuma subcultura, nenhum ``parent``, nenhuma identidade híbrida;
* na composição nacional e nas cidades, minorias passam a ser a cultura
  nacional do país de origem (diáspora/imigração), nunca uma cultura hifenizada.

Regras de mapeamento (determinísticas, sem inventar números):

* grupo interno de uma nação (iorubá na Nigéria, zulu na África do Sul, han na
  China) funde-se na cultura nacional daquela nação;
* o mesmo grupo em outro país vira a cultura nacional do seu país de origem
  (punjabi em Londres -> indiano; turco em Berlim -> turco);
* ``outras`` absorve o que não tem nação de origem clara na curadoria (ex.:
  "caribenho" fora do Caribe, "melanésio" na Nova Caledônia).

Uso (uma vez, sobre o catálogo v1.2)::

    <python> tools/migrate_cultures_v2.py
"""

from __future__ import annotations

import csv
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
SOURCES = Path("tools/culture_sources")
SCENARIO = Path("data/definitions/scenario")

OTHER = "outras"

# --- catálogo nacional -------------------------------------------------------
# (tag, id, pt, en, es, família, herda de)
# ``herda de`` fornece idioma, religião e (quando a família está vazia) família
# do grupo titular antigo; não é usado pela UI, só mantém o dado preenchido sem
# inventar valores novos.
NATION_ROWS = [
    ("AFG", "afegao", "Afegão", "Afghan", "afgano", "", "pashto"),
    ("AGO", "angolano", "Angolano", "Angolan", "angoleño", "bantu central", "ovimbundu"),
    ("ALB", "albanes", "Albanês", "Albanian", "albanés", "", "albanes"),
    ("ARE", "emiratense", "Emiratense", "Emirati", "emiratí", "árabe peninsular", "arabe_peninsular"),
    ("ARG", "argentino", "Argentino", "Argentine", "argentino", "", "argentino"),
    ("ARM", "armenio", "Armênio", "Armenian", "armenio", "", "armenio"),
    ("ATG", "antiguano", "Antiguano", "Antiguan", "antiguano", "", "antiguano"),
    ("AUS", "australiano", "Australiano", "Australian", "australiano", "", "australiano"),
    ("AUT", "austriaco", "Austríaco", "Austrian", "austríaco", "", "alemao"),
    ("AZE", "azerbaijano", "Azerbaijano", "Azerbaijani", "azerbaiyano", "", "azeri"),
    ("BDI", "burundines", "Burundinês", "Burundian", "burundés", "bantu oriental", "grandes_lagos"),
    ("BEL", "belga", "Belga", "Belgian", "belga", "", "flamengo"),
    ("BEN", "beninense", "Beninense", "Beninese", "beninés", "golfo-da-guiné", "fon"),
    ("BFA", "burquinense", "Burquinense", "Burkinabé", "burkinés", "golfo-da-guiné", "mossi"),
    ("BGD", "bengali", "Bengali", "Bangladeshi", "bangladesí", "", "bengali"),
    ("BGR", "bulgaro", "Búlgaro", "Bulgarian", "búlgaro", "", "bulgaro"),
    ("BHR", "bareinita", "Bareinita", "Bahraini", "bareiní", "árabe peninsular", "arabe_peninsular"),
    ("BHS", "bahamense", "Baamense", "Bahamian", "bahamés", "", "bahamense"),
    ("BIH", "bosnio", "Bósnio", "Bosnian", "bosnio", "", "bosnio"),
    ("BLR", "bielorrusso", "Bielorrusso", "Belarusian", "bielorruso", "", "bielorrusso"),
    ("BLZ", "belizense", "Belizense", "Belizean", "beliceño", "", "belizense"),
    ("BOL", "boliviano", "Boliviano", "Bolivian", "boliviano", "", "boliviano"),
    ("BRA", "brasileiro", "Brasileiro", "Brazilian", "brasileño", "", "brasileiro"),
    ("BRB", "barbadense", "Barbadense", "Barbadian", "barbadense", "", "barbadense"),
    ("BRN", "bruneano", "Bruneano", "Bruneian", "bruneano", "", "malaio"),
    ("BTN", "butanes", "Butanês", "Bhutanese", "butanés", "", "tibetano"),
    ("BWA", "botsuanense", "Botsuanense", "Botswanan", "botsuano", "bantu oriental", "tsuana"),
    ("CAF", "centro_africano", "Centro-africano", "Central African", "centroafricano", "bantu central", "sango"),
    ("CAN", "canadense", "Canadense", "Canadian", "canadiense", "", "canadense_ingles"),
    ("CHE", "suico", "Suíço", "Swiss", "suizo", "", "alemao"),
    ("CHL", "chileno", "Chileno", "Chilean", "chileno", "", "chileno"),
    ("CHN", "chines", "Chinês", "Chinese", "chino", "", "han"),
    ("CIV", "marfinense", "Marfinense", "Ivorian", "marfileño", "golfo-da-guiné", "akan"),
    ("CMR", "camaronense", "Camaronense", "Cameroonian", "camerunés", "bantu central", "fang"),
    ("COD", "congoles_rdc", "Congolês (RDC)", "Congolese (DRC)", "congoleño (RDC)", "bantu central", "lingala"),
    ("COG", "congoles", "Congolês", "Congolese", "congoleño", "bantu central", "kikongo"),
    ("COL", "colombiano", "Colombiano", "Colombian", "colombiano", "", "colombiano"),
    ("CRI", "costa_riquenho", "Costa-riquenho", "Costa Rican", "costarricense", "", "costa_riquenho"),
    ("CUB", "cubano", "Cubano", "Cuban", "cubano", "", "cubano"),
    ("CYP", "cipriota", "Cipriota", "Cypriot", "chipriota", "", "grego"),
    ("CZE", "tcheco", "Tcheco", "Czech", "checo", "", "tcheco"),
    ("DEU", "alemao", "Alemão", "German", "alemán", "", "alemao"),
    ("DJI", "djibutiense", "Djibutiense", "Djiboutian", "yibutí", "chifre-africano", "somali"),
    ("DMA", "dominiquense", "Dominiquense", "Dominican (Dominica)", "dominiqués", "", "dominiquense"),
    ("DNK", "dinamarques", "Dinamarquês", "Danish", "danés", "", "dinamarques"),
    ("DOM", "dominicano", "Dominicano", "Dominican", "dominicano", "", "dominicano"),
    ("DZA", "argelino", "Argelino", "Algerian", "argelino", "árabe magrebina", "arabe_magrebino"),
    ("ECU", "equatoriano", "Equatoriano", "Ecuadorian", "ecuatoriano", "", "equatoriano"),
    ("EGY", "egipcio", "Egípcio", "Egyptian", "egipcio", "árabe nilótica", "arabe_egipcio"),
    ("ERI", "eritreu", "Eritreu", "Eritrean", "eritreo", "chifre-africano", "tigrinio"),
    ("ESP", "espanhol", "Espanhol", "Spanish", "español", "", "castelhano"),
    ("EST", "estoniano", "Estoniano", "Estonian", "estonio", "", "estoniano"),
    ("ETH", "etiope", "Etíope", "Ethiopian", "etíope", "chifre-africano", "oromo"),
    ("FIN", "finlandes", "Finlandês", "Finnish", "finlandés", "", "finlandes"),
    ("FJI", "fijiano", "Fijiano", "iTaukei", "fiyiano", "", "fijiano"),
    ("FRA", "frances", "Francês", "French", "francés", "", "frances"),
    ("GAB", "gabonense", "Gabonense", "Gabonese", "gabonés", "bantu central", "fang"),
    ("GBR", "britanico", "Britânico", "British", "británico", "", "ingles"),
    ("GEO", "georgiano", "Georgiano", "Georgian", "georgiano", "", "georgiano"),
    ("GHA", "ganense", "Ganense", "Ghanaian", "ghanés", "golfo-da-guiné", "akan"),
    ("GIN", "guineense", "Guineense", "Guinean", "guineano", "golfo-da-guiné", "fulani"),
    ("GMB", "gambiano", "Gambiano", "Gambian", "gambiano", "golfo-da-guiné", "wolof"),
    ("GNB", "bissauguineense", "Bissau-guineense", "Bissau-Guinean", "bisauguineano", "golfo-da-guiné", "crioulo"),
    ("GNQ", "equatoguineense", "Equatoguineense", "Equatoguinean", "ecuatoguineano", "bantu central", "fang"),
    ("GRC", "grego", "Grego", "Greek", "griego", "", "grego"),
    ("GRD", "granadino", "Granadino", "Grenadian", "granadino", "", "granadino"),
    ("GTM", "guatemalteco", "Guatemalteco", "Guatemalan", "guatemalteco", "", "guatemalteco"),
    ("GUY", "guianense", "Guianense", "Guyanese", "guyanés", "", "guianense"),
    ("HND", "hondurenho", "Hondurenho", "Honduran", "hondureño", "", "hondurenho"),
    ("HRV", "croata", "Croata", "Croatian", "croata", "", "croata"),
    ("HTI", "haitiano", "Haitiano", "Haitian", "haitiano", "", "haitiano"),
    ("HUN", "hungaro", "Húngaro", "Hungarian", "húngaro", "", "hungaro"),
    ("IDN", "indonesio", "Indonésio", "Indonesian", "indonesio", "", "javanes"),
    ("IND", "indiano", "Indiano", "Indian", "indio", "", "hindustani"),
    ("IRL", "irlandes", "Irlandês", "Irish", "irlandés", "", "irlandes"),
    ("IRN", "iraniano", "Iraniano", "Iranian", "iraní", "", "persa"),
    ("IRQ", "iraquiano", "Iraquiano", "Iraqi", "iraquí", "árabe levantina", "arabe_iraquiano"),
    ("ISL", "islandes", "Islandês", "Icelandic", "islandés", "", "islandes"),
    ("ISR", "israelense", "Israelense", "Israeli", "israelí", "", "hebraico"),
    ("ITA", "italiano", "Italiano", "Italian", "italiano", "", "italiano"),
    ("JAM", "jamaicano", "Jamaicano", "Jamaican", "jamaicano", "", "jamaicano"),
    ("JOR", "jordaniano", "Jordaniano", "Jordanian", "jordano", "árabe levantina", "arabe_levantino"),
    ("JPN", "japones", "Japonês", "Japanese", "japonés", "", "japones"),
    ("KAZ", "cazaque", "Cazaque", "Kazakh", "kazajo", "", "cazaque"),
    ("KEN", "queniano", "Queniano", "Kenyan", "keniano", "bantu oriental", "kikuyu"),
    ("KGZ", "quirguiz", "Quirguiz", "Kyrgyz", "kirguís", "", "quirguiz"),
    ("KHM", "cambojano", "Cambojano", "Cambodian", "camboyano", "", "khmer"),
    ("KNA", "sancristovense", "São-cristovense", "Kittitian", "sancristobaleño", "", "sancristovense"),
    ("KOR", "sul_coreano", "Sul-coreano", "South Korean", "surcoreano", "", "coreano"),
    ("KWT", "kuwaitiano", "Kuwaitiano", "Kuwaiti", "kuwaití", "árabe peninsular", "arabe_peninsular"),
    ("LAO", "laosiano", "Laosiano", "Lao", "laosiano", "", "lao"),
    ("LBN", "libanes", "Libanês", "Lebanese", "libanés", "árabe levantina", "arabe_levantino"),
    ("LBR", "liberiano", "Liberiano", "Liberian", "liberiano", "golfo-da-guiné", "kru"),
    ("LBY", "libio", "Líbio", "Libyan", "libio", "árabe magrebina", "arabe_magrebino"),
    ("LCA", "santa_luciense", "Santa-luciense", "Saint Lucian", "santalucense", "", "santa_luciense"),
    ("LKA", "srilankes", "Srilankês", "Sri Lankan", "esrilanqués", "", "cingales"),
    ("LSO", "lesotense", "Lesotense", "Basotho", "lesotense", "bantu oriental", "soto"),
    ("LTU", "lituano", "Lituano", "Lithuanian", "lituano", "", "lituano"),
    ("LUX", "luxemburgues", "Luxemburguês", "Luxembourgish", "luxemburgués", "", "luxemburgues"),
    ("LVA", "leto", "Letão", "Latvian", "letón", "", "leto"),
    ("MAR", "marroquino", "Marroquino", "Moroccan", "marroquí", "árabe magrebina", "arabe_magrebino"),
    ("MDA", "moldavio", "Moldávio", "Moldovan", "moldavo", "", "romeno"),
    ("MDG", "malgaxe", "Malgaxe", "Malagasy", "malgache", "", "malgaxe"),
    ("MEX", "mexicano", "Mexicano", "Mexican", "mexicano", "", "mexicano"),
    ("MKD", "macedonio", "Macedônio", "Macedonian", "macedonio", "", "macedonio"),
    ("MLI", "malines", "Malinês", "Malian", "maliense", "golfo-da-guiné", "bambara"),
    ("MLT", "maltes", "Maltês", "Maltese", "maltés", "", "maltes"),
    ("MMR", "birmanes", "Birmanês", "Burmese", "birmano", "", "birmanes"),
    ("MNE", "montenegrino", "Montenegrino", "Montenegrin", "montenegrino", "", "montenegrino"),
    ("MNG", "mongol", "Mongol", "Mongolian", "mongol", "", "mongol"),
    ("MOZ", "mocambicano", "Moçambicano", "Mozambican", "mozambiqueño", "bantu oriental", "macua"),
    ("MRT", "mauritano", "Mauritano", "Mauritanian", "mauritano", "árabe magrebina", "arabe_magrebino"),
    ("MWI", "malauiano", "Malauiano", "Malawian", "malauí", "bantu oriental", "chewa"),
    ("MYS", "malasio", "Malásio", "Malaysian", "malasio", "", "malaio"),
    ("NAM", "namibiano", "Namibiano", "Namibian", "namibio", "bantu oriental", "ovambo"),
    ("NED", "neerlandes", "Neerlandês", "Dutch", "neerlandés", "", "neerlandes"),
    ("NER", "nigerino", "Nigerino", "Nigerien", "nigerino", "golfo-da-guiné", "hausea"),
    ("NGA", "nigeriano", "Nigeriano", "Nigerian", "nigeriano", "golfo-da-guiné", "hausea"),
    ("NIC", "nicaraguense", "Nicaraguense", "Nicaraguan", "nicaragüense", "", "nicaraguense"),
    ("NOR", "noruegues", "Norueguês", "Norwegian", "noruego", "", "noruegues"),
    ("NPL", "nepales", "Nepalês", "Nepali", "nepalí", "", "nepali"),
    ("NZL", "neozelandes", "Neozelandês", "New Zealander", "neozelandés", "", "neozelandes"),
    ("OMN", "omanense", "Omanense", "Omani", "omaní", "árabe peninsular", "arabe_peninsular"),
    ("PAK", "paquistanes", "Paquistanês", "Pakistani", "paquistaní", "", "urdu"),
    ("PAN", "panamenho", "Panamenho", "Panamanian", "panameño", "", "panamenho"),
    ("PER", "peruano", "Peruano", "Peruvian", "peruano", "", "peruano"),
    ("PHL", "filipino", "Filipino", "Filipino", "filipino", "", "filipino"),
    ("PNG", "papuano", "Papua-nova-guineense", "Papua New Guinean", "papú-neoguineano", "papua", "papua"),
    ("POL", "polones", "Polonês", "Polish", "polaco", "", "polones"),
    ("PRK", "norte_coreano", "Norte-coreano", "North Korean", "norcoreano", "", "coreano"),
    ("PRT", "portugues", "Português", "Portuguese", "portugués", "", "portugues"),
    ("PRY", "paraguaio", "Paraguaio", "Paraguayan", "paraguayo", "", "paraguaio"),
    ("PSX", "palestino", "Palestino", "Palestinian", "palestino", "árabe levantina", "arabe_levantino"),
    ("QAT", "catariano", "Catariano", "Qatari", "catarí", "árabe peninsular", "arabe_peninsular"),
    ("ROU", "romeno", "Romeno", "Romanian", "rumano", "", "romeno"),
    ("RUS", "russo", "Russo", "Russian", "ruso", "", "russo"),
    ("RWA", "ruandes", "Ruandês", "Rwandan", "ruandés", "bantu oriental", "grandes_lagos"),
    ("SAH", "saaraui", "Saarauí", "Sahrawi", "saharaui", "árabe magrebina", "arabe_magrebino"),
    ("SAU", "saudita", "Saudita", "Saudi", "saudí", "árabe peninsular", "arabe_peninsular"),
    ("SDN", "sudanes", "Sudanês", "Sudanese", "sudanés", "árabe nilótica", "arabe_sudanes"),
    ("SEN", "senegales", "Senegalês", "Senegalese", "senegalés", "golfo-da-guiné", "wolof"),
    ("SGP", "singapurense", "Singapurense", "Singaporean", "singapurense", "austronésia", "han"),
    ("SLB", "salomonico", "Salomônico", "Solomon Islander", "salomonense", "", "melanesio"),
    ("SLE", "serra_leones", "Serra-leonês", "Sierra Leonean", "sierraleonés", "golfo-da-guiné", "kru"),
    ("SLV", "salvadorenho", "Salvadorenho", "Salvadoran", "salvadoreño", "", "salvadorenho"),
    ("SOL", "somalilandes", "Somalilandês", "Somalilander", "somalilandés", "chifre-africano", "somali"),
    ("SOM", "somali", "Somali", "Somali", "somalí", "chifre-africano", "somali"),
    ("SRB", "servio", "Sérvio", "Serbian", "serbio", "", "servio"),
    ("SSD", "sul_sudanes", "Sul-sudanês", "South Sudanese", "sursudanés", "chifre-africano", "dinka"),
    ("SUR", "surinames", "Surinamês", "Surinamese", "surinamés", "caribenha", "hindustani"),
    ("SVK", "eslovaco", "Eslovaco", "Slovak", "eslovaco", "", "eslovaco"),
    ("SVN", "esloveno", "Esloveno", "Slovene", "esloveno", "", "esloveno"),
    ("SWE", "sueco", "Sueco", "Swedish", "sueco", "", "sueco"),
    ("SWZ", "suaziliense", "Suaziliense", "Swazi", "suazi", "bantu oriental", "suazi"),
    ("SYR", "sirio", "Sírio", "Syrian", "sirio", "árabe levantina", "arabe_levantino"),
    ("TCD", "chadiano", "Chadiano", "Chadian", "chadiano", "árabe nilótica", "arabe_chadiano"),
    ("TGO", "togoles", "Togolês", "Togolese", "togolés", "golfo-da-guiné", "ewe"),
    ("THA", "tailandes", "Tailandês", "Thai", "tailandés", "", "tailandes"),
    ("TJK", "tadjique", "Tadjique", "Tajik", "tayiko", "", "tadjique"),
    ("TKM", "turcomeno", "Turcomeno", "Turkmen", "turcomano", "", "turcomeno"),
    ("TLS", "timorense", "Timorense", "Timorese", "timorense", "", "timorense"),
    ("TTO", "trinitario", "Trinitário", "Trinidadian", "trinitense", "", "trinitario"),
    ("TUN", "tunisiano", "Tunisiano", "Tunisian", "tunecino", "árabe magrebina", "arabe_magrebino"),
    ("TUR", "turco", "Turco", "Turkish", "turco", "", "turco"),
    ("TWN", "taiwanes", "Taiwanês", "Taiwanese", "taiwanés", "sino-tibetana", "han"),
    ("TZA", "tanzaniano", "Tanzaniano", "Tanzanian", "tanzano", "bantu oriental", "suaili"),
    ("UGA", "ugandense", "Ugandense", "Ugandan", "ugandés", "bantu oriental", "ganda"),
    ("UKR", "ucraniano", "Ucraniano", "Ukrainian", "ucraniano", "", "ucraniano"),
    ("URY", "uruguaio", "Uruguaio", "Uruguayan", "uruguayo", "", "uruguaio"),
    ("USA", "americano", "Americano", "American", "estadounidense", "", "americano"),
    ("UZB", "uzbeque", "Uzbeque", "Uzbek", "uzbeko", "", "uzbeque"),
    ("VCT", "sao_vicentino", "São-vicentino", "Saint Vincentian", "sanvicentino", "", "sao_vicentino"),
    ("VEN", "venezuelano", "Venezuelano", "Venezuelan", "venezolano", "", "venezuelano"),
    ("VNM", "vietnamita", "Vietnamita", "Vietnamese", "vietnamita", "", "vietnamita"),
    ("XKX", "kosovar", "Kosovar", "Kosovar", "kosovar", "albanesa", "albanes"),
    ("YEM", "iemenita", "Iemenita", "Yemeni", "yemení", "árabe peninsular", "arabe_peninsular"),
    ("ZAF", "sul_africano", "Sul-africano", "South African", "sudafricano", "bantu oriental", "zulu"),
    ("ZMB", "zambiano", "Zambiano", "Zambian", "zambiano", "bantu oriental", "bemba"),
    ("ZWE", "zimbabuense", "Zimbabuense", "Zimbabwean", "zimbabuense", "bantu oriental", "shona"),
]

# --- povos sem estado --------------------------------------------------------
# (id, pt, en, es, família, idioma, religião, home_region, nota)
STATELESS_ROWS = [
    ("curdo", "Curdo", "Kurdish", "kurdo", "iraniana", "curdo", "islã sunita",
     "TUR-DIY;IRQ-ERB;IRN-KSH", "povo sem estado no cenário (Curdistão dividido)"),
    ("catalao", "Catalão", "Catalan", "catalán", "latina ibérica", "catalão", "catolicismo",
     "ESP-BAR;ESP-VAL;AND-AND", "povo sem estado (Catalunha)"),
    ("basco", "Basco", "Basque", "vasco", "isolada", "basco", "catolicismo",
     "ESP;FRA", "povo sem estado (País Basco); o cenário não tem cidade-núcleo basca"),
    ("quebequense", "Quebequense", "Québécois", "quebequés", "latina francesa",
     "francês (quebequense)", "catolicismo", "CAN-MON;CAN-QUE", "povo sem estado (Quebec)"),
    ("uigur", "Uigur", "Uyghur", "uigur", "turcomana", "uigur", "islã sunita",
     "CHN-URU;CHN-KAS", "povo sem estado (Xinjiang)"),
    ("tibetano", "Tibetano", "Tibetan", "tibetano", "sino-tibetana", "tibetano", "budismo",
     "CHN-LHA", "povo sem estado (Tibete)"),
    ("tamil", "Tâmil", "Tamil", "tamil", "dravidiana", "tâmil", "hinduísmo",
     "LKA-COL;LKA-JAF", "povo sem estado no Sri Lanka (Tâmil Eelam)"),
    ("tartaro_crimeia", "Tártaro da Crimeia", "Crimean Tatar", "tártaro de Crimea",
     "turcomana", "tártaro da Crimeia", "islã sunita", "RUS-SEV",
     "povo sem estado (crimeus deportados; Crimeia sob a Rússia no cenário)"),
    ("berbere", "Berbere (amazigue)", "Berber", "bereber", "berbere", "tamazigue", "islã sunita",
     "MAR-CAS;MAR-RAK;MAR-AGA;DZA-ALG", "povos amazigues sem estado"),
    ("tuaregue", "Tuaregue", "Tuareg", "tuareg", "berbere", "tamasheq", "islã sunita",
     "MLI-GAO;NER-AGA;DZA-TIN", "povo sem estado (Saara/Sael)"),
    ("checheno", "Checheno", "Chechen", "checheno", "caucasiana", "checheno", "islã sunita",
     "RUS", "povo sem estado (Chechênia)"),
    ("tatar", "Tártaro", "Tatar", "tártaro", "turcomana", "tártaro", "islã sunita",
     "RUS-KAZ", "povo sem estado (Tataristão); distinto do tártaro da Crimeia"),
    ("afar", "Afar", "Afar", "afar", "cuxítica", "afar", "islã sunita",
     "DJI-DJI;ETH-WAR", "povo sem estado (Depressão de Afar)"),
    ("balochi", "Belúchi", "Baloch", "baluchi", "iraniana", "balúchi", "islã sunita",
     "PAK-QUE;IRN", "povo sem estado (Baluchistão)"),
    ("papua", "Papua (Nova Guiné Ocidental)", "Papuan (Western New Guinea)",
     "papú (Nueva Guinea Occidental)", "papua", "línguas papuas", "cristianismo",
     "IDN-WAM", "povos papuas sob a Indonésia; distinto do nacional de Papua-Nova Guiné"),
    ("maori", "Maori", "Māori", "maorí", "austronésia", "maori", "cristianismo",
     "NZL-AUC;NZL-WEL;NZL-CHR", "povo sem estado (Aotearoa)"),
    ("aborigene", "Aborígene australiano", "Aboriginal Australian", "aborigen australiano",
     "aborígene", "línguas aborígenes", "espiritualidades indígenas", "AUS-ALI;AUS-DAR",
     "povos aborígenes da Austrália"),
    ("havaiano", "Havaiano", "Native Hawaiian", "nativo hawaiano", "polinésia", "havaiano",
     "espiritualidades indígenas", "USA-HON", "povo sem estado (Havaí)"),
    ("inuit", "Inuit-iúpik", "Inuit-Yupik", "inuit-yupik", "esquimó-aleúte",
     "inuktitut/iúpik", "cristianismo",
     "CAN-IQA;CAN-KUU;CAN-IGL;CAN-RAN;CAN-INU;USA-UTQ;USA-BET", "povos inuit/iúpik"),
    ("nativo_americano", "Nativo americano", "American Indian/Alaska Native",
     "nativo americano", "ameríndia", "diversas línguas indígenas",
     "espiritualidades indígenas", "USA-ANC;USA-BET;USA-SIT;USA-UNA;USA-UTQ",
     "nações indígenas dos EUA"),
    ("indigena_brasileiro", "Indígena brasileiro", "Brazilian Indigenous",
     "indígena brasileño", "ameríndia", "línguas indígenas brasileiras",
     "espiritualidades indígenas", "BRA-MAN;BRA-BOA", "povos indígenas do Brasil"),
    ("quechua", "Quíchua", "Quechua", "quechua", "ameríndia", "quíchua", "catolicismo",
     "PER-ARE;PER-IQU;PER-TRU;BOL-COC;ECU-GUA", "povos quíchuas"),
    ("aimara", "Aimará", "Aymara", "aimara", "ameríndia", "aimará", "catolicismo",
     "BOL-LAP;PER-ARE", "povo aimará"),
    ("guarani", "Guarani", "Guarani", "guaraní", "ameríndia", "guarani", "catolicismo",
     "PRY-ASU;PRY-CIU", "povo guarani"),
    ("maia", "Maia", "Maya", "maya", "ameríndia", "línguas maias", "catolicismo",
     "GTM-GUA;GTM-COB;MEX-MER;MEX-CAN", "povos maias"),
    ("mapuche", "Mapuche", "Mapuche", "mapuche", "ameríndia", "mapudungun", "catolicismo",
     "CHL-ANT;CHL-PUN;ARG-NEU", "povo mapuche"),
    ("nahua", "Nahua", "Nahua", "nahua", "ameríndia", "náuatle", "catolicismo",
     "MEX-MEXI;MEX-PUE", "povos nahuas"),
]

# --- mapeamento das culturas antigas ----------------------------------------
# Grupos que são o próprio povo da nação em qualquer contexto: fundem-se na
# cultura nacional do país onde aparecem.
ALWAYS_INTERNAL = {
    "amhara", "oromo", "tigrinio", "hausea", "ioruba", "igbo", "fulani", "wolof",
    "akan", "bambara", "songai", "kanuri", "ewe", "fon", "mossi", "sango",
    "crioulo", "suaili", "kikuyu", "ganda", "luo", "zulu", "xhosa", "soto",
    "tsuana", "shona", "chewa", "kikongo", "lingala", "luba", "bemba", "mbundu",
    "ovimbundu", "ovambo", "herero", "tsonga", "macua", "suazi", "ndebele",
    "fang", "malgaxe", "grandes_lagos", "kru", "javanes", "sunda", "batak",
    "minangkabau", "bugis", "moro", "khmer",
}

# Cultura nacional antiga -> nova cultura nacional por omissão.
NATIONAL_MAP = {
    "russo": "russo", "ucraniano": "ucraniano", "bielorrusso": "bielorrusso",
    "polones": "polones", "tcheco": "tcheco", "eslovaco": "eslovaco",
    "servio": "servio", "croata": "croata", "bosnio": "bosnio",
    "esloveno": "esloveno", "macedonio": "macedonio", "bulgaro": "bulgaro",
    "montenegrino": "montenegrino",
    "lituano": "lituano", "leto": "leto", "estoniano": "estoniano",
    "finlandes": "finlandes", "hungaro": "hungaro", "alemao": "alemao",
    "neerlandes": "neerlandes", "flamengo": "belga", "afrikaans": "sul_africano",
    "ingles": "britanico", "escoces": "britanico", "gales": "britanico",
    "norte_irlandes": "britanico", "irlandes": "irlandes",
    "dinamarques": "dinamarques", "noruegues": "noruegues", "sueco": "sueco",
    "islandes": "islandes", "frances": "frances", "valao": "belga",
    "italiano": "italiano", "romeno": "romeno", "grego": "grego",
    "albanes": "albanes", "castelhano": "espanhol", "galego": "espanhol",
    "portugues": "portugues", "maltes": "maltes", "turco": "turco",
    "azeri": "azerbaijano", "georgiano": "georgiano", "armenio": "armenio",
    "persa": "iraniano", "tadjique": "tadjique", "pashto": "afegao",
    "turcomeno": "turcomeno", "cazaque": "cazaque", "uzbeque": "uzbeque",
    "quirguiz": "quirguiz", "mongol": "mongol",
    "arabe_levantino": "libanes", "arabe_egipcio": "egipcio",
    "arabe_magrebino": "marroquino", "arabe_peninsular": "saudita",
    "arabe_iraquiano": "iraquiano", "arabe_sudanes": "sudanes",
    "arabe_chadiano": "chadiano", "hebraico": "israelense", "somali": "somali",
    "hindustani": "indiano", "urdu": "paquistanes", "punjabi": "indiano",
    "bengali": "bengali", "gujarati": "indiano", "marata": "indiano",
    "cingales": "srilankes", "nepali": "nepales", "telugu": "indiano",
    "malaio": "malasio", "filipino": "filipino", "tailandes": "tailandes",
    "lao": "laosiano", "vietnamita": "vietnamita", "birmanes": "birmanes",
    "han": "chines", "cantones": "chines", "hokkien": "chines",
    "hakka": "chines", "teochew": "chines",
    "japones": "japones", "coreano": "sul_coreano", "fijiano": "fijiano",
    "timorense": "timorense", "brasileiro": "brasileiro", "mexicano": "mexicano",
    "americano": "americano", "mexicano_americano": "mexicano",
    "canadense_ingles": "canadense", "caribenho": "jamaicano",
    "guatemalteco": "guatemalteco", "hondurenho": "hondurenho",
    "salvadorenho": "salvadorenho", "nicaraguense": "nicaraguense",
    "costa_riquenho": "costa_riquenho", "panamenho": "panamenho",
    "cubano": "cubano", "dominicano": "dominicano",
    "porto_riquenho": "americano", "colombiano": "colombiano",
    "venezuelano": "venezuelano", "equatoriano": "equatoriano",
    "peruano": "peruano", "boliviano": "boliviano", "chileno": "chileno",
    "argentino": "argentino", "uruguaio": "uruguaio", "paraguaio": "paraguaio",
    "dinka": "sul_sudanes", "nuer": "sul_sudanes", "belizense": "belizense",
    "guianense": "guianense", "haitiano": "haitiano", "jamaicano": "jamaicano",
    "trinitario": "trinitario", "bahamense": "bahamense",
    "barbadense": "barbadense", "antiguano": "antiguano",
    "dominiquense": "dominiquense", "granadino": "granadino",
    "sancristovense": "sancristovense", "santa_luciense": "santa_luciense",
    "sao_vicentino": "sao_vicentino", "canares": "indiano", "malaiala": "indiano",
    "australiano": "australiano", "neozelandes": "neozelandes",
    "luxemburgues": "luxemburgues", "melanesio": "",
}

# Cultura antiga -> {tag: novo id} quando o contexto muda o destino.
INTERNAL_BY_TAG = {
    "alemao": {"DEU": "alemao", "AUT": "austriaco", "CHE": "suico", "LUX": "luxemburgues"},
    "ingles": {"GBR": "britanico", "ZAF": "sul_africano", "IRL": "irlandes"},
    "frances": {"FRA": "frances", "BEL": "belga", "CHE": "suico", "CAN": "quebequense",
                "HTI": "haitiano"},
    "italiano": {"ITA": "italiano", "CHE": "suico"},
    "sueco": {"SWE": "sueco", "FIN": "finlandes"},
    "romeno": {"ROU": "romeno", "MDA": "moldavio"},
    "grego": {"GRC": "grego", "CYP": "cipriota"},
    "albanes": {"ALB": "albanes", "XKX": "kosovar"},
    "tadjique": {"TJK": "tadjique", "AFG": "afegao"},
    "uzbeque": {"UZB": "uzbeque", "AFG": "afegao"},
    "turcomeno": {"TKM": "turcomeno", "AFG": "afegao"},
    "mongol": {"MNG": "mongol", "CHN": "chines"},
    "pashto": {"AFG": "afegao", "PAK": "paquistanes"},
    "arabe_levantino": {"JOR": "jordaniano", "SYR": "sirio", "LBN": "libanes",
                        "PSX": "palestino", "ISR": "palestino"},
    "arabe_magrebino": {"DZA": "argelino", "MAR": "marroquino", "TUN": "tunisiano",
                        "LBY": "libio", "MRT": "mauritano", "SAH": "saaraui"},
    "arabe_peninsular": {"SAU": "saudita", "ARE": "emiratense", "QAT": "catariano",
                         "KWT": "kuwaitiano", "BHR": "bareinita", "OMN": "omanense",
                         "YEM": "iemenita"},
    "somali": {"SOM": "somali", "SOL": "somalilandes", "DJI": "djibutiense",
               "ETH": "etiope", "KEN": "queniano"},
    "hindustani": {"IND": "indiano", "PAK": "paquistanes", "NPL": "nepales"},
    "urdu": {"PAK": "paquistanes", "IND": "indiano", "GBR": "paquistanes"},
    "punjabi": {"PAK": "paquistanes", "IND": "indiano", "GBR": "indiano",
                "CAN": "indiano", "ARE": "paquistanes"},
    "bengali": {"BGD": "bengali", "IND": "indiano"},
    "gujarati": {"IND": "indiano"},
    "marata": {"IND": "indiano"},
    "telugu": {"IND": "indiano"},
    "canares": {"IND": "indiano"},
    "malaiala": {"IND": "indiano"},
    "cingales": {"LKA": "srilankes"},
    "nepali": {"NPL": "nepales", "BTN": "nepales"},
    "tamil": {"LKA": "tamil", "IND": "indiano", "MYS": "indiano",
              "SGP": "singapurense"},
    "han": {"CHN": "chines", "TWN": "taiwanes", "SGP": "singapurense"},
    "cantones": {"CHN": "chines", "TWN": "taiwanes", "SGP": "singapurense",
                 "MYS": "chines"},
    "hokkien": {"CHN": "chines", "TWN": "taiwanes", "SGP": "singapurense",
                "MYS": "chines"},
    "hakka": {"CHN": "chines", "TWN": "taiwanes", "SGP": "singapurense",
              "MYS": "chines"},
    "teochew": {"CHN": "chines", "TWN": "taiwanes", "SGP": "singapurense",
                "MYS": "chines"},
    "malaio": {"MYS": "malasio", "BRN": "bruneano", "IDN": "indonesio",
               "SGP": "singapurense", "THA": "tailandes", "TLS": "timorense"},
    "javanes": {"IDN": "indonesio"},
    "sunda": {"IDN": "indonesio"},
    "batak": {"IDN": "indonesio"},
    "minangkabau": {"IDN": "indonesio"},
    "bugis": {"IDN": "indonesio"},
    "coreano": {"KOR": "sul_coreano", "PRK": "norte_coreano"},
    "melanesio": {"SLB": "salomonico", "PNG": "papuano"},
    "papua": {"PNG": "papuano", "SLB": "salomonico", "FRA": ""},
    "caribenho": {"SUR": "surinames", "GBR": "jamaicano", "NED": "neerlandes"},
    "afrikaans": {"ZAF": "sul_africano", "NAM": "namibiano"},
    "tibetano": {"BTN": "butanes"},
    "khmer": {"KHM": "cambojano", "VNM": "vietnamita", "THA": "tailandes",
              "LAO": "laosiano"},
    "ingles_phl": {},
}

# Destinos explícitos por (cultura antiga, tag), quando o padrão não serve.
OVERRIDES = {
    ("ingles", "PHL"): "",
    ("malaio", "TLS"): "timorense",
}

# Povos sem estado: o id antigo já é o novo id.
STATELESS_IDS = {row[0]: row[0] for row in STATELESS_ROWS}


def national_ids() -> dict[str, str]:
    return {row[0]: row[1] for row in NATION_ROWS}


def resolve(old: str, tag: str, nations: dict[str, str]) -> str:
    """Destino de ``old`` no contexto da nação ``tag`` ("" = vai para outras)."""
    key = (old, tag)
    if key in OVERRIDES:
        return OVERRIDES[key]
    if old in INTERNAL_BY_TAG and tag in INTERNAL_BY_TAG[old]:
        return INTERNAL_BY_TAG[old][tag]
    if old in ALWAYS_INTERNAL:
        return nations.get(tag, "")
    if old in STATELESS_IDS:
        return STATELESS_IDS[old]
    if old in NATIONAL_MAP:
        return NATIONAL_MAP[old]
    raise SystemExit(f"cultura antiga sem mapeamento: {old!r}")


def parse_mix(text: str) -> list[tuple[str, float]]:
    entries = []
    for raw in (text or "").split(";"):
        raw = raw.strip()
        if not raw:
            continue
        cid, share = raw.split(":", 1)
        entries.append((cid.strip(), float(share)))
    return entries


def merge(entries: list[tuple[str, float]]) -> dict[str, float]:
    merged: dict[str, float] = {}
    for cid, share in entries:
        merged[cid] = merged.get(cid, 0.0) + share
    return merged


def map_mix(text: str, tag: str, nations: dict[str, str], known: set[str],
            require_total: bool = True) -> dict[str, float]:
    mapped: list[tuple[str, float]] = []
    for cid, share in parse_mix(text):
        if cid == OTHER:
            mapped.append((OTHER, share))
            continue
        new = resolve(cid, tag, nations)
        if new:
            if new not in known:
                raise SystemExit(f"{tag}: destino desconhecido {new!r} (de {cid!r})")
            mapped.append((new, share))
        else:
            mapped.append((OTHER, share))
    merged = merge(mapped)
    total = sum(merged.values())
    if total > 100.05:
        raise SystemExit(f"{tag}: mix soma {total:g} > 100")
    if require_total and abs(total - 100.0) > 0.05:
        if OTHER not in merged:
            raise SystemExit(f"{tag}: mix soma {total:g} sem 'outras'")
        merged[OTHER] = merged[OTHER] + (100.0 - total)
    if merged.get(OTHER, 0.0) <= 0.001:
        merged.pop(OTHER, None)
    return merged


def format_mix(mix: dict[str, float]) -> str:
    ordered = sorted(((cid, share) for cid, share in mix.items() if cid != OTHER),
                     key=lambda item: (-item[1], item[0]))
    if OTHER in mix:
        ordered.append((OTHER, mix[OTHER]))
    parts = []
    for cid, share in ordered:
        value = f"{share:.1f}".rstrip("0").rstrip(".")
        parts.append(f"{cid}:{value}")
    return ";".join(parts)


def main() -> int:
    if hasattr(sys.stdout, "reconfigure"):
        sys.stdout.reconfigure(encoding="utf-8", errors="replace")
    cultures_path = SOURCES / "cultures.csv"
    old_rows = list(csv.DictReader(cultures_path.open(encoding="utf-8", newline="")))
    old_by_id = {row["id"]: row for row in old_rows}
    old_nations = {row["nation"]: row["primary_culture"]
                   for row in csv.DictReader((SOURCES / "nation_cultures.csv").open(encoding="utf-8", newline=""))}
    national = national_ids()
    scenario_city_nation = {row["id"]: row["nation"] for row in csv.DictReader(
        (SCENARIO / "cities.csv").open(encoding="utf-8", newline=""))}
    known = set(national.values()) | set(STATELESS_IDS)

    # 1) catálogo novo ------------------------------------------------------
    catalog = []
    for tag, cid, pt, en, es, family, inherit in NATION_ROWS:
        source = old_by_id[inherit]
        catalog.append({
            "id": cid,
            "name_pt": pt,
            "name_en": en,
            "name_es": es,
            "family": family or source["family"],
            "language": source["language"],
            "religion": source["religion"],
            "nation": tag,
            "home_region": "",
            "notes": f"cultura nacional de {tag}",
        })
    for cid, pt, en, es, family, language, religion, home, note in STATELESS_ROWS:
        catalog.append({
            "id": cid,
            "name_pt": pt,
            "name_en": en,
            "name_es": es,
            "family": family,
            "language": language,
            "religion": religion,
            "nation": "",
            "home_region": home,
            "notes": note,
        })

    ids = [row["id"] for row in catalog]
    if len(ids) != len(set(ids)):
        raise SystemExit("catálogo novo: ids duplicados")
    if len(national) != len(NATION_ROWS):
        raise SystemExit("tabela nacional: tags duplicadas")
    for tag in old_nations:
        if tag not in national:
            raise SystemExit(f"nação sem cultura nacional na tabela: {tag}")
    for tag in national:
        if tag not in old_nations:
            raise SystemExit(f"tabela nacional cita nação fora do cenário: {tag}")

    # 2) composições nacionais ---------------------------------------------
    nation_rows = []
    for row in csv.DictReader((SOURCES / "nation_cultures.csv").open(encoding="utf-8", newline="")):
        tag = row["nation"]
        mix = map_mix(row["composition"], tag, national, known)
        if national[tag] not in mix:
            raise SystemExit(f"{tag}: cultura nacional fora da composição")
        accepted = []
        for cid in filter(None, (row["accepted_cultures"] or "").split(";")):
            new = resolve(cid.strip(), tag, national)
            if new and new != national[tag] and new not in accepted:
                accepted.append(new)
        nation_rows.append({
            "nation": tag,
            "primary_culture": national[tag],
            "accepted_cultures": ";".join(accepted),
            "composition": format_mix(mix),
            "source": row["source"],
            "year": row["year"],
        })

    # 3) overrides de cidade -----------------------------------------------
    city_files = sorted(SOURCES.glob("city_overrides_*.csv"))
    city_rows_by_file = {}
    for path in city_files:
        by_city: dict[str, list[dict]] = {}
        for row in csv.DictReader(path.open(encoding="utf-8", newline="")):
            by_city.setdefault(row["city_id"], []).append(row)
        out = []
        for city_id, rows in by_city.items():
            tag = scenario_city_nation.get(city_id, "")
            if not tag:
                raise SystemExit(f"{path.name}: cidade fora do cenário {city_id!r}")
            mix = map_mix(";".join(f"{r['culture']}:{r['share']}" for r in rows), tag, national, known,
                          require_total=False)
            sources = []
            for r in rows:
                if r["source"] not in sources:
                    sources.append(r["source"])
            for cid, share in sorted(((c, s) for c, s in mix.items() if c != OTHER),
                                     key=lambda item: (-item[1], item[0])):
                out.append({
                    "city_id": city_id,
                    "culture": cid,
                    "share": f"{share:.1f}".rstrip("0").rstrip("."),
                    "source": " + ".join(sources),
                })
            if OTHER in mix:
                out.append({
                    "city_id": city_id,
                    "culture": OTHER,
                    "share": f"{mix[OTHER]:.1f}".rstrip("0").rstrip("."),
                    "source": " + ".join(sources),
                })
        city_rows_by_file[path] = out

    # 4) gravar -------------------------------------------------------------
    with cultures_path.open("w", encoding="utf-8", newline="") as fh:
        fields = ["id", "name_pt", "name_en", "name_es", "family", "language",
                  "religion", "nation", "home_region", "notes"]
        writer = csv.DictWriter(fh, fieldnames=fields, lineterminator="\n")
        writer.writeheader()
        writer.writerows(catalog)

    with (SOURCES / "nation_cultures.csv").open("w", encoding="utf-8", newline="") as fh:
        writer = csv.DictWriter(fh, fieldnames=["nation", "primary_culture",
                                                "accepted_cultures", "composition",
                                                "source", "year"],
                                lineterminator="\n")
        writer.writeheader()
        writer.writerows(nation_rows)

    for path, rows in city_rows_by_file.items():
        with path.open("w", encoding="utf-8", newline="") as fh:
            writer = csv.DictWriter(fh, fieldnames=["city_id", "culture", "share", "source"],
                                    lineterminator="\n")
            writer.writeheader()
            writer.writerows(rows)

    print(f"catálogo: {len(catalog)} culturas "
          f"({len(NATION_ROWS)} nacionais + {len(STATELESS_ROWS)} sem estado)")
    print(f"nações: {len(nation_rows)}")
    print(f"cidades com override: {sum(len({r['city_id'] for r in rows}) for rows in city_rows_by_file.values())}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
