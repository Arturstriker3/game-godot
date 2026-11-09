# Métricas objetivas das capturas

Método: `pixel-metrics`. Inspeção visual por modelo: **não**.

> Estas são estatísticas de pixels, não julgamento estético.

| imagem | brilho | contraste | saturação | nitidez | bordas |
| --- | --- | --- | --- | --- | --- |
| map3d_political_d013.png | 0.436 | 0.379 | 0.368 | 0.00352 | 0.067 |
| map3d_political_d026.png | 0.382 | 0.419 | 0.419 | 0.00502 | 0.156 |
| map3d_political_d052.png | 0.330 | 0.525 | 0.484 | 0.00899 | 0.227 |
| map3d_political_d120.png | 0.346 | 0.881 | 0.429 | 0.00746 | 0.123 |

## Classes de pixels (água × terra)

| imagem | água | sat. água | lum. água | hex água | sat. terra | lum. terra |
| --- | --- | --- | --- | --- | --- | --- |
| map3d_political_d013.png | 0.253 | 0.498 | 0.270 | #2B4B55 | 0.324 | 0.492 |
| map3d_political_d026.png | 0.346 | 0.513 | 0.242 | #26434D | 0.369 | 0.456 |
| map3d_political_d052.png | 0.547 | 0.513 | 0.223 | #253D47 | 0.448 | 0.459 |
| map3d_political_d120.png | 0.620 | 0.466 | 0.177 | #1F3138 | 0.368 | 0.622 |

## Comparações

| A | B | MAE | RMSE | SSIM | Δbrilho | pixels alterados |
| --- | --- | --- | --- | --- | --- | --- |
| map3d_political_d052.png | map3d_political_d052.png | 0.0044 | 0.0075 | 1.0034 | 0.0051 | 0.0% |
| map3d_political_d120.png | map3d_political_d120.png | 0.0038 | 0.0071 | 0.9974 | 0.0041 | 0.0% |
