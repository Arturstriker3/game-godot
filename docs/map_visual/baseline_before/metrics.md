# Métricas objetivas das capturas

Método: `pixel-metrics`. Inspeção visual por modelo: **não**.

> Estas são estatísticas de pixels, não julgamento estético.

| imagem | brilho | contraste | saturação | nitidez | bordas |
| --- | --- | --- | --- | --- | --- |
| flat_geographic_s062.png | 0.227 | 0.404 | 0.972 | 0.00084 | 0.049 |
| flat_geographic_s125.png | 0.341 | 0.872 | 0.842 | 0.00337 | 0.115 |
| flat_geographic_s250.png | 0.325 | 0.837 | 0.855 | 0.00428 | 0.141 |
| flat_geographic_s500.png | 0.389 | 0.914 | 0.770 | 0.00409 | 0.128 |
| flat_political_s062.png | 0.226 | 0.401 | 0.970 | 0.00085 | 0.051 |
| flat_political_s125.png | 0.340 | 0.871 | 0.841 | 0.00353 | 0.124 |
| flat_political_s250.png | 0.324 | 0.835 | 0.854 | 0.00443 | 0.145 |
| flat_political_s500.png | 0.389 | 0.914 | 0.769 | 0.00411 | 0.128 |
| legacy2d_political_d013.png | 0.622 | 0.118 | 0.550 | 0.00012 | 0.005 |
| legacy2d_political_d026.png | 0.654 | 0.264 | 0.479 | 0.00039 | 0.010 |
| legacy2d_political_d052.png | 0.683 | 0.339 | 0.429 | 0.00081 | 0.029 |
| legacy2d_political_d120.png | 0.676 | 0.288 | 0.407 | 0.00298 | 0.079 |
| map3d_geographic_d013.png | 0.437 | 0.324 | 0.738 | 0.00465 | 0.081 |
| map3d_geographic_d026.png | 0.410 | 0.342 | 0.806 | 0.00617 | 0.203 |
| map3d_geographic_d052.png | 0.396 | 0.609 | 0.819 | 0.01055 | 0.260 |
| map3d_geographic_d120.png | 0.405 | 0.911 | 0.637 | 0.00888 | 0.144 |
| map3d_political_d013.png | 0.460 | 0.286 | 0.543 | 0.00449 | 0.063 |
| map3d_political_d026.png | 0.422 | 0.346 | 0.661 | 0.00584 | 0.166 |
| map3d_political_d052.png | 0.398 | 0.608 | 0.777 | 0.01000 | 0.247 |
| map3d_political_d120.png | 0.405 | 0.911 | 0.631 | 0.00895 | 0.147 |

## Comparações

| A | B | MAE | RMSE | SSIM | Δbrilho | pixels alterados |
| --- | --- | --- | --- | --- | --- | --- |
| map3d_political_d052.png | flat_political_s250.png | 0.3013 | 0.3880 | 0.1840 | 0.0740 | 95.9% |
| map3d_political_d052.png | legacy2d_political_d052.png | 0.3685 | 0.4275 | 0.3472 | -0.2843 | 100.0% |
| map3d_political_d120.png | flat_political_s500.png | 0.3258 | 0.4399 | 0.2297 | 0.0160 | 90.9% |
| map3d_geographic_d052.png | flat_geographic_s250.png | 0.3020 | 0.3886 | 0.1836 | 0.0707 | 95.7% |
