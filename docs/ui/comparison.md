# Métricas objetivas das capturas

Método: `pixel-metrics`. Inspeção visual por modelo: **não**.

> Estas são estatísticas de pixels, não julgamento estético.

| imagem | brilho | contraste | saturação | nitidez | bordas |
| --- | --- | --- | --- | --- | --- |
| 00_hud.png | 0.159 | 0.351 | 0.645 | 0.00541 | 0.046 |
| 01_economy_nations.png | 0.096 | 0.129 | 0.401 | 0.00436 | 0.032 |
| 02_economy_cities.png | 0.097 | 0.129 | 0.401 | 0.00551 | 0.034 |
| 03_economy_empty.png | 0.096 | 0.129 | 0.401 | 0.00447 | 0.031 |
| 04_trade.png | 0.098 | 0.129 | 0.401 | 0.00764 | 0.035 |
| 05_production.png | 0.096 | 0.129 | 0.401 | 0.00358 | 0.025 |
| 06_politics.png | 0.096 | 0.129 | 0.401 | 0.00367 | 0.028 |
| 07_city.png | 0.097 | 0.129 | 0.401 | 0.00392 | 0.027 |
| 08_menu.png | 0.084 | 0.133 | 0.534 | 0.00250 | 0.017 |

## Classes de pixels (água × terra)

| imagem | água | sat. água | lum. água | hex água | sat. terra | lum. terra |
| --- | --- | --- | --- | --- | --- | --- |
| 00_hud.png | 0.636 | 0.790 | 0.131 | #0F262D | 0.392 | 0.208 |
| 01_economy_nations.png | 0.178 | 0.549 | 0.093 | #11191D | 0.369 | 0.097 |
| 02_economy_cities.png | 0.178 | 0.549 | 0.093 | #11191D | 0.369 | 0.098 |
| 03_economy_empty.png | 0.178 | 0.549 | 0.093 | #10191D | 0.369 | 0.097 |
| 04_trade.png | 0.177 | 0.552 | 0.093 | #10191D | 0.369 | 0.099 |
| 05_production.png | 0.175 | 0.551 | 0.093 | #11191D | 0.370 | 0.096 |
| 06_politics.png | 0.175 | 0.549 | 0.094 | #111A1D | 0.370 | 0.096 |
| 07_city.png | 0.174 | 0.554 | 0.094 | #111A1D | 0.369 | 0.097 |
| 08_menu.png | 0.623 | 0.673 | 0.072 | #0A1418 | 0.304 | 0.104 |

## Comparações

| A | B | MAE | RMSE | SSIM | Δbrilho | pixels alterados |
| --- | --- | --- | --- | --- | --- | --- |
| 00_hud.png | 00_hud.png | 0.0029 | 0.0305 | 0.9689 | -0.0000 | 1.4% |
| 01_economy_nations.png | 01_economy_nations.png | 0.0194 | 0.0481 | 0.8220 | 0.0005 | 6.0% |
| 04_trade.png | 04_trade.png | 0.0216 | 0.0576 | 0.8049 | 0.0003 | 6.3% |
| 07_city.png | 07_city.png | 0.0194 | 0.0485 | 0.8384 | 0.0005 | 6.1% |
