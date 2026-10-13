# Resposta ao feedback de produção — lakehouse DuckLake (2026-10)

> Respondendo ao feedback do consumidor (lakehouse DuckLake, DuckDB 1.5.6, 21
> models bronze, carga incremental delete+insert a cada 2 h). Formato: o que
> já existe hoje (e como usar), o que entra em qual versão, e custo de API
> esperado para P1/P2 em objetos de 10+ M linhas.

## 1. O achado da doc — vocês estão certos, e a correção já está mapeada

A frase **"deletes do not update SystemModstamp"** estava **errada**. A
medição de vocês (8.576 de 8.576 deletados em 15 dias carregados só pelo
watermark) confirma o comportamento real do Salesforce: **deleção atualiza o
`SystemModstamp`** para o horário da exclusão, e a linha deletada permanece
visível via `queryAll` (`IsDeleted = true`) até a purga da lixeira (~15
dias). O próprio fallback `queryAll` do `salesforce_deleted_ids` usa esse
padrão como filtro — a frase da doc o contradizia.

O que o watermark **de fato** não pega:

- **Purga**: linha deletada cuja lixeira foi esvaziada antes da próxima
  carga (carga com intervalo > 15 dias, ou purge manual) — some do
  `queryAll` e não volta;
- **Objetos sem `IsDeleted`** (ex.: `User`) — o fallback `queryAll` falha
  com `INVALID_FIELD`;
- **Timestamp de deleção exato** — `SystemModstamp` de linha deletada fica
  com o valor antigo? Não: fica com o horário da exclusão, mas o
  `getDeleted()` devolve `deletedDate` dedicado e cobre o pós-purga.

Correção da frase entra na **v0.19.2**: manuais EN/PT (2 seções), usage
guide (11.1/12.1), SKILL, e a **descrição da função dentro do binário**
(`duckdb_functions()` — o texto do `salesforce_deleted_ids` também carrega a
frase errada hoje). O job de purge-safety que vocês prepararam é o desenho
certo; com a correção da doc ele deixa de parecer redundante.

## 2. O que já existe hoje (v0.19.1) e como usar

### Delete-sync (confirmado por vocês — fechado)

```sql
-- janela estreita: getDeleted, deletedDate exato (User incluído)
SELECT id, deleted_date FROM salesforce_deleted_ids('sf', 'Lead',
    since := '2026-09-30T00:00:00', until := '2026-09-30T00:15:00');

-- janela larga: getDeleted também (sem corte de 15 min)
SELECT id, deleted_date FROM salesforce_deleted_ids('sf', 'Lead',
    since := '2026-09-15T00:00:00');
```

Os 160 mil ids em ~60 s batem com o desenho (1 chamada getDeleted por slice
de 15 min, sem paginação REST).

### Varredura de chaves (P2) — o caminho barato é o Bulk, e ele já existe

Projeção só de `Id` **não** tem caminho automático especial hoje: com
transporte REST (default), 10 M linhas = **~5.000 chamadas** (10 M / 2.000
por página) e horas de stream. O caminho barato é explícito:

```sql
-- 1 job Bulk v2 (não consome DailyApiRequests; consome 1 dos 10.000
-- Bulk V2 Query Jobs/dia da org de vocês), server-side, sem PK chunking
-- necessário para Id-only:
SELECT "Id" FROM salesforce_scan('sf', 'Embalagem__History',
    transport := 'bulk');

-- se o run morrer no meio: o job já está completo no Salesforce
SELECT "Id", "Name" FROM salesforce_bulk_resume('sf', '7504x00000ABCDef');
-- (retoma as páginas de resultado sem reexecutar a query)
```

O custo aparece em `salesforce_query_cost()` (`transport = 'bulk'`,
`est_rows`, `pages_fetched`). Para paridade de chave: 1 job Bulk por censo
(+ `salesforce_bulk_resume` para retomada) é o desenho recomendado — o
reconcile por pai com `IN` de 1.000 ids continua melhor via REST (1 call
com `IN` pushdown por lote).

### Retenção de History (P3) — a data mínima já se obtém hoje

```sql
SELECT * FROM salesforce_aggregate('sf', 'AccountHistory',
    'MIN(CreatedDate) mais_antigo, COUNT(Id) n_linhas');
```

Pushdown server-side, 1 chamada. Serve para a paridade comparar só a janela
retida (a diferença 1,4×–2,8× que vocês mediram é a retenção de 18 meses
purgando o lado da org — documentação entra na v0.19.2/v0.20.0).

## 3. O que entra em qual versão (proposta)

### v0.19.2 — patch rápido (sem GO ainda)

1. **Correção da frase** em todos os pontos (manuais EN/PT, usage guide
   EN/PT, SKILL, e a descrição do `salesforce_deleted_ids` dentro do
   binário via `duckdb_functions()`);
2. **P4** — `salesforce_deleted_ids` ganha a coluna
   `latest_date_covered` (fonte getDeleted): se a janela pedida passou do
   que a org cobriu, o consumidor vê o buraco em vez de assumir "não houve
   exclusões";
3. **P5a** — `ATTACH ... (READ_ONLY)` aceito e ignorado (a extensão já é
   read-only; nota nos docs);
4. **P5b** — validação local de formato de Id (15/18 caracteres) em
   predicados `=`/`IN` sobre campos `id`, com erro claro
   (`INVALID_QUERY_FILTER_OPERATOR` vira mensagem nossa apontando o
   formato);
5. **P1 mitigação** — `sf_timeout_read_s` (timeout de leitura HTTP
   configurável; hoje fixo em 60 s, com 3 tentativas ≈ 3-4 min até o erro).

Esforço: pequeno (validação/doc + 1 setting). Tudo mock-testável.

### v0.20.0 — minor (ciclo seguinte)

1. **P1 solução completa — COUNT chunked por faixa de Id**: named param
   `count_chunks := N` no `salesforce_scan`/count de objetos grandes — N
   faixas de `Id`, um `SELECT COUNT() FROM obj WHERE Id >= a AND Id < b`
   por faixa (cada uma server-side e rápida), soma local. Bulk API 2.0
   **não** aceita `COUNT()` (agregados não são suportados no Bulk query) —
   por isso o chunking é via REST mesmo;
2. **P2/P3/P5-3 docs** — padrão key sweep via Bulk; retenção de History
   (18 meses) e como distingui-la de perda; boas práticas para objetos de
   churn alto (watermark + delete-sync + compactação no destino).

### Backlog (sem data)

- Função dedicada de census PK-chunked com retomada própria;
- `where_in(col, lista)` com fatiamento interno (paridade com o firebird),
  se o padrão de 1.000 ids por lote ainda incomodar.

## 4. Custo de API esperado (P1/P2, 10 M+ linhas)

Referência da org de vocês: `DailyApiRequests` 141,8 k/dia; `Bulk V2 Query
Jobs` 10 k/dia.

### P2 — varredura de chaves (Id-only, objeto inteiro)

| Caminho | Custo | Tempo esperado |
|---|---|---|
| `salesforce_scan(..., transport := 'bulk')` | **1 job Bulk v2** (1/10.000 do dia) + downloads de resultado (não contam como API calls) | 10 M ids: job processa em minutos; download de centenas de páginas |
| REST (`transport := 'rest'`) | ~5.000 DailyApiRequests por sweep completo | horas |

**Recomendação**: censo de chaves via Bulk (1 job por censo; semanal ou
mensal), paridade fina via `salesforce_deleted_ids` (janela estreita,
~1 call) — praticamente zero custo contínuo.

### P1 — COUNT de objeto grande

| Caminho | Custo | Risco |
|---|---|---|
| Hoje: `COUNT()` pushdown REST | **1 call** (1/141,8 k do dia) | Salesforce pode levar > 60 s em History de 16 M sob `queryAll` → timeout de leitura (falha em ~3-4 min após retries) |
| v0.19.2: `sf_timeout_read_s = 300` | mesma 1 call, esperando mais | resolve a maioria dos casos |
| v0.20.0: COUNT chunked | ~100-200 COUNT() por faixa de Id (cada uma em segundos), soma local | sem timeout de servidor; custo proporcional às faixas |

**Recomendação prática para o censo diário**: paridade principal = contagem
no lake (vocês já têm) + delta via `salesforce_deleted_ids` (60 s, ~1 call).
Censo REST completo (chunked) só no cadastro semanal/mensal.

## 5. Resumo

| Item | Estado | Versão |
|---|---|---|
| Doc SystemModstamp (errada) | Correção mapeada (5 pontos + descrição in-binário) | v0.19.2 |
| P4 — cobertura do deleted_ids | Coluna `latest_date_covered` | v0.19.2 |
| P5a — `READ_ONLY` no ATTACH | Aceito e ignorado | v0.19.2 |
| P5b — validação de Id 15/18 | Erro claro local | v0.19.2 |
| P1 mitigação — timeout configurável | `sf_timeout_read_s` | v0.19.2 |
| P1 solução — COUNT chunked | Desenho fechado | v0.20.0 |
| P2 — key sweep barato | **Já existe** (`transport := 'bulk'` + `bulk_resume`) | docs |
| P3 — retenção History | **Já existe** (`MIN(CreatedDate)` via aggregate) | docs |
