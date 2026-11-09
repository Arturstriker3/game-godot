# Interface de batalha — referência (Hearts of Iron IV)

Referência visual da tela de batalha do HOI4 (Paradox). O que nos interessa não é
a guerra em si (fora do escopo atual), mas a **composição da tela**: como um
confronto entre duas forças é apresentado de forma legível e cinematográfica.

**Imagem:** `reference.webp` — captura da tela de batalha do HOI4 (1080×606, 149 KB).

---

## 1. Leitura da tela

A tela é um **modal de tela cheia** sobre o mapa, que escurece o fundo e mantém a
guerra visível ao redor. Dentro dele, a informação se organiza em cinco zonas:

1. **Cabeçalho**: local da batalha ("batalha na região de Aachen") e o brasão do
   país atacante. Fica dentro de uma moldura de metal.
2. **Faixa superior de clima/terreno**: duas imagens cinematográficas — uma do
   atacante (tanque em chamas) e uma do defensor (aviões) — ladeando uma **barra
   vertical de progresso** em gradiente (verde → amarelo → vermelho) com os
   rótulos "Attack" e "Defend".
3. **Colunas laterais**: lista de divisões de cada lado, com ícone de tipo,
   nome, barras de **força** e **organização**, e indicadores de apoio (ex.:
   apoio aéreo, vantagem defensiva).
4. **Centro**: **mapa tático** da região, em pergaminho, com contadores de
   unidade (símbolos NATO), setas de ataque vermelhas, rios, cidades, fumaça e
   explosões; acima, os **retratos dos comandantes** dos dois lados e a
   **largura de combate** (número "20").
5. **Rodapé**: duas barras longas (vermelha/azul) resumindo o total de cada lado.

Fora do painel, à direita, um **retrato 3D do general** em tamanho grande, com
medalhas e espada — âncora emocional/propaganda.

## 2. Linguagem visual

- **Moldura de metal escuro** com rebites, cantos ornamentados e vidro fumê:
  dá peso institucional ao confronto.
- **Pergaminho/cartografia** no mapa tático: contraste com o metal e leitura
  histórica.
- **Bandeiras saturadas** como únicos acentos de cor forte.
- **Barras de status** verde/amarelo/vermelho — leitura instantânea de vantagem.
- **Tipografia condensada** em caixa alta, com hierarquia clara (título da
  batalha > nomes de divisão > detalhes).
- **Iluminação cinematográfica**: fumaça, fogo, vinheta; o mapa parece um
  diorama, não um gráfico.
- **Informação em camadas**: o resumo (quem ataca, quem defende, quem está
  ganhando) salta; o detalhe (divisões, largura, terreno) fica disponível.

## 3. Por que funciona

- **Comparação lado a lado**: atacante à esquerda, defensor à direita, mesma
  estrutura — o cérebro compara sem esforço.
- **Um número-herói**: a barra central de progresso responde "quem está
  ganhando?" antes de qualquer leitura.
- **Contexto espacial**: o mapa tático mostra *onde*, não só *quanto*.
- **Rosto humano**: os retratos transformam números em personagens.
- **Enquadramento**: o modal não cobre o mapa inteiro; a guerra continua
  visível atrás, mantendo a sensação de tempo real.

## 4. O que aproveitar no OpenGS (mesmo sem guerra)

O **esqueleto** serve para qualquer confronto ou negociação:

- Modal com **cabeçalho + 3 colunas + rodapé**.
- **Comparação lado a lado** (nós x eles) com as mesmas métricas dos dois lados.
- **Barra central de resultado** como número-herói.
- **Mapa tático** (nossa mapa 3D, recortado ou em miniatura) para ancorar o
  evento no espaço.
- **Retrato/âncora visual** de um personagem (prefeito, governador, ministro).
- **Barras de status** com a paleta verde/amarelo/vermelho já usada no HUD.

Aplicações futuras possíveis, sem exército:

- Disputa de mercado entre nações (ofertas, tarifas, embargo).
- Negociação comercial ou diplomática com prazo e "placar".
- Relatório de crise (greve, escassez, desastre) com lados e consequências.
- Evento eleitoral ou de influência política.

## 5. Estrutura sugerida em Godot (quando for implementar)

- `Control` full-rect com fundo escurecido (`ColorRect` alpha ~0.55) e captura
  de input.
- Painel central: `NinePatchRect` com a moldura de metal (tema do design system).
- `HBoxContainer` com três colunas (esquerda flexível, centro fixo, direita
  flexível).
- Listas: `VBoxContainer` + item reutilizável (`division_row.tscn`) com ícone,
  nome e duas `ProgressBar` temáticas.
- Barra central: `ProgressBar` vertical ou duas barras sobrepostas.
- Mapa tático: `SubViewport` com a câmera do mapa principal, ou `TextureRect`
  com um recorte do atlas.
- Retrato: `TextureRect` grande, ancorado fora do painel, com vinheta.
- i18n: todos os rótulos via `I18n.t()`; o layout deve aguentar pt-BR, en e es.

## 6. O que NÃO replicar

- Densidade de números do HOI4: nossa economia é menor; mostrar menos.
- Contadores militares (símbolos NATO) — não há exército no escopo.
- Texturas/arte do jogo original.

---

*Referência analisada a partir da imagem compartilhada pelo usuário. É um estudo
de composição, não uma cópia de interface.*
