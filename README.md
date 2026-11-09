# Open Grand Strategy (WarEconomy)

Simulação de grand strategy 3D focada em **economia e logística**: o comércio de
trigo entre cidades reais da Europa Ocidental (produção, demanda, estoque, frete e
tempo de trânsito por caminhão, navio e avião), com uma interface de comando densa
inspirada em Hearts of Iron III.

- **Engine:** Godot 4.7 (Forward+) + núcleo econômico em C++17 via GDExtension (`EconomySim`).
- **Estado:** protótipo jogável — HUD completo, economia de trigo ao vivo, mapa de
  satélite 3D, crescimento urbano/industrial modular e um cenário de validação do
  ciclo econômico completo (AGRIA/PORTUS).

## Como rodar

```powershell
& "C:\caminho\Godot_v4.7.2-stable_win64.exe" --path .
```

## Documentação

- [`GAME_STATE.md`](GAME_STATE.md) — fonte central de conhecimento (visão, arquitetura, sistemas, decisões, bugs e roadmap).
- [`DESIGN.md`](DESIGN.md) — design system da interface.
- `docs/` — relatórios e evidências (UI, mapa, economia).

## Licença

MIT.
