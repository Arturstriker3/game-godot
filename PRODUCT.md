# Product

<!-- impeccable:product-schema 1 -->

## Platform

desktop (Godot 4.7, Forward+; interface em nós `Control`, não web)

## Stack

C++17 via GDExtension (núcleo econômico `EconomySim`) + Godot 4.7 para render e UI.

## Users

Jogadores de grand strategy que já conhecem Paradox/Supreme Ruler e esperam ler o estado do mundo em painéis densos, sem tutorial. A sessão típica é longa: observar transporte de carga, comparar nações, pausar e acelerar o tempo.

## Product Purpose

Simular comércio de trigo entre cidades reais na Europa, com produção, demanda, estoque, frete e tempo de trânsito por caminhão, navio e avião. A interface existe para tornar essa economia legível e operável: o jogador precisa ver onde falta trigo, de onde vem o socorro e quanto custa.

## Positioning

O mapa e a economia são os mesmos objetos: cada cidade é uma fonte de produção e um ponto de demanda real, e as fronteiras das nações nascem da posição dessas cidades (Voronoi), não de províncias pintadas à mão.

## Operating Context

- 1900×1000 px, tela cheia de desktop, sessão contínua com pausa e velocidades.
- O tempo avança em dias; a UI mostra dia e hora.
- Dados reais disponíveis por tick: 5 nações, 5 cidades, população, produção diária, demanda, estoque, déficit, carga em trânsito, pedidos de comércio com trechos e frete.
- Não existem ainda: exército, guerra, tecnologia, política interna, construção de fábricas, dinheiro por nação.

## Capabilities and Constraints

- Telas necessárias: barra superior persistente, painel de nações/cidades, detalhe de cidade, comércio/logística com detalhe de pedido, produção/construção e política/diplomacia/dossiê.
- Internacionalização obrigatória em pt-BR, en e es, trocável em tempo de execução.
- Produção e política não têm dados de simulação: devem aparecer como estado vazio convincente, nunca com números inventados.
- A UI deve continuar legível a 1600×900 e 1920×1080.

## Brand Commitments

- Referência declarada pelo usuário: interface de Hearts of Iron III, em versão modernizada (mesma linguagem de painéis escuros e latão, com tipografia e espaçamento mais legíveis).
- Idioma padrão: pt-BR, com en e es disponíveis.

## Evidence on Hand

- `assets/fonts/IMPRISHA.TTF` (fonte serifada de display já no projeto).
- `assets/models/city.glb`, `truck.glb`, `ship.glb`, `plane.glb` e o mapa de satélite `map/map_data/satellite.jpg`.
- Núcleo C++ expõe `get_status()` e `tick()` com os campos listados em Operating Context.

## Product Principles

1. O dado vem antes do ornamento: cada painel responde a uma pergunta econômica concreta.
2. Nada de número inventado: onde a simulação não tem dado, a tela diz o que falta.
3. Densidade legível: muita informação por tela, com hierarquia tipográfica clara.
4. O mapa é a superfície principal; a interface é uma camada por cima dele.
5. Trocar de idioma não quebra layout nem rótulos.

## Accessibility & Inclusion

Texto principal mínimo de 13 px (cabeçalhos de coluna podem ir a 11 px em caixa alta), contraste alto sobre painéis escuros, alvos de clique com no mínimo 28 px de altura e estado de foco visível.
