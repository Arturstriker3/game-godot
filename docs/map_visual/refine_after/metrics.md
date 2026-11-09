# Métricas objetivas das capturas

Método: `pixel-metrics`. Inspeção visual por modelo: **não**.

> Estas são estatísticas de pixels, não julgamento estético.

| imagem | brilho | contraste | saturação | nitidez | bordas |
| --- | --- | --- | --- | --- | --- |
| map3d_political_d013.png | 0.439 | 0.404 | 0.369 | 0.00354 | 0.067 |
| map3d_political_d026.png | 0.382 | 0.440 | 0.420 | 0.00505 | 0.154 |
| map3d_political_d052.png | 0.327 | 0.533 | 0.484 | 0.00891 | 0.225 |
| map3d_political_d120.png | 0.344 | 0.882 | 0.429 | 0.00747 | 0.131 |

## Classes de pixels (água × terra)

| imagem | água | sat. água | lum. água | hex água | sat. terra | lum. terra |
| --- | --- | --- | --- | --- | --- | --- |
| map3d_political_d013.png | 0.253 | 0.497 | 0.270 | #2B4B55 | 0.325 | 0.496 |
| map3d_political_d026.png | 0.348 | 0.512 | 0.242 | #26434D | 0.371 | 0.457 |
| map3d_political_d052.png | 0.549 | 0.512 | 0.223 | #253E47 | 0.451 | 0.454 |
| map3d_political_d120.png | 0.621 | 0.466 | 0.177 | #1F3138 | 0.369 | 0.617 |

## Comparações

| A | B | MAE | RMSE | SSIM | Δbrilho | pixels alterados |
| --- | --- | --- | --- | --- | --- | --- |
| map3d_political_d120.png | map3d_political_d120.png | 0.1125 | 0.1733 | 0.7964 | 0.0607 | 64.5% |
| map3d_political_d052.png | map3d_political_d052.png | 0.1404 | 0.1824 | 0.7887 | 0.0711 | 82.1% |
| map3d_political_d026.png | map3d_political_d026.png | 0.1151 | 0.1533 | 0.8550 | 0.0399 | 62.6% |
| map3d_political_d013.png | map3d_political_d013.png | 0.0926 | 0.1336 | 0.9011 | 0.0217 | 43.7% |
