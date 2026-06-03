# duckdb-salesforce - Guia de uso para analistas

Este guia mostra como consultar uma org Salesforce diretamente do DuckDB
usando a extensao `duckdb-salesforce`. A extensao anexa sua org como um
**catalogo somente-leitura**: objetos Salesforce (sObjects) aparecem como
tabelas das quais voce faz `SELECT` com SQL comum, enquanto a extensao
traduz sua consulta em SOQL e escolhe a API correta do Salesforce (REST ou
Bulk) nos bastidores.

E pratico e baseado em exemplos. Cada configuracao e funcao citada aqui e
real; nada foi inventado.

## Referencias oficiais DuckDB

Este guia segue conceitos documentados pelo DuckDB:

- [`ATTACH`](https://duckdb.org/docs/current/sql/statements/attach.html):
  anexa outro catalogo ao DuckDB. Esta extensao usa o mesmo modelo para
  expor uma org Salesforce como catalogo somente-leitura.
- [`SELECT`](https://duckdb.org/docs/stable/sql/statements/select):
  o unico comando que voce executa contra o catalogo Salesforce. Toda
  escrita e DDL lanca erro.
- [`CREATE TABLE`](https://duckdb.org/docs/stable/sql/statements/create_table):
  `CREATE TABLE ... AS SELECT` materializa uma consulta Salesforce em uma
  tabela local do DuckDB.
- [Table functions](https://duckdb.org/docs/stable/sql/functions/overview):
  esta extensao fornece funcoes de diagnostico como
  `salesforce_query_cost()`.

## 1. Conceitos antes de comecar

### Catalogo somente-leitura

`ATTACH` expoe a org como catalogo. Voce so pode ler dele: todo `INSERT`,
`UPDATE`, `DELETE` e DDL contra o catalogo Salesforce lanca erro. Se voce
precisa de uma copia local gravavel, materialize uma consulta em uma tabela
DuckDB (veja a secao 5).

### Tabelas sao sObjects

Cada tabela no catalogo anexado e um sObject do Salesforce (`Account`,
`Contact`, `Opportunity`, objetos personalizados terminados em `__c`, e
assim por diante). O schema e resolvido de forma **preguicosa** (lazy) na
primeira vez que voce referencia um objeto, entao anexar e barato e rapido.

### Autenticacao e seguranca

A autenticacao e **OAuth 2.0 apenas por refresh-token**. Voce fornece um
`client_id`, um `client_secret` e um `refresh_token`. As credenciais ficam
somente em memoria e nunca sao registradas em log. A verificacao de
certificado TLS esta sempre ativa.

## 2. Instalacao e carregamento

Um build local nao e **assinado**, entao o DuckDB nao o carrega sem que
voce permita explicitamente:

```sql
SET allow_unsigned_extensions=true;
LOAD 'caminho/para/salesforce.duckdb_extension';
```

Para os passos de build e instalacao por plataforma, veja
[docs/en/guide_windows.md](../en/guide_windows.md) (Windows) e
[docs/en/guide_linux.md](../en/guide_linux.md) (Linux).

As plataformas validadas no CI hoje sao `linux_amd64` e `windows_amd64`. A
extensao ainda nao foi publicada no catalogo community, entao ainda nao
existe um `INSTALL ... FROM community`.

## 3. Conectar ao Salesforce

Anexe a org com o tipo de catalogo `salesforce` e suas credenciais OAuth:

```sql
ATTACH 'salesforce://<org>' AS sf (
    TYPE salesforce,
    client_id 'sua_consumer_key',
    client_secret 'seu_consumer_secret',
    refresh_token 'seu_refresh_token'
);
```

Dois parametros opcionais cobrem sandboxes e versoes de API fixadas:

```sql
ATTACH 'salesforce://<org>' AS sf (
    TYPE salesforce,
    client_id 'sua_consumer_key',
    client_secret 'seu_consumer_secret',
    refresh_token 'seu_refresh_token',
    login_url 'https://login.salesforce.com',
    api_version 'v60.0'
);
```

Use `login_url 'https://test.salesforce.com'` para uma sandbox. Para obter
o `client_id`, o `client_secret` e o `refresh_token`, configure um
Connected App na sua org e conclua o fluxo OAuth conforme descrito em
[docs/connected_app.md](../connected_app.md).

## 4. Primeiras consultas

Uma vez anexada, consulte sObjects como qualquer outra tabela:

```sql
SELECT Id, Name
FROM sf.Account
WHERE Name = 'Acme'
LIMIT 10;
```

O schema e resolvido lazy na primeira referencia. Para listar os objetos
que voce pode consultar, execute um unico describe global via:

```sql
SHOW TABLES;
-- ou
SELECT * FROM duckdb_tables();
```

## 5. Materializar resultados localmente

Para manter um snapshot local e consultavel (e fugir das limitacoes de
somente-leitura), copie uma consulta Salesforce para uma tabela DuckDB:

```sql
CREATE TABLE contas_locais AS
SELECT Id, Name, Industry, AnnualRevenue
FROM sf.Account
WHERE Industry = 'Manufacturing';
```

A partir dai voce pode consultar, juntar e agregar `contas_locais` sem
novas chamadas de API.

## 6. Transportes: REST, Bulk e auto

A extensao pode buscar linhas por diferentes APIs do Salesforce. Escolha
com `sf_force_transport`:

```sql
-- REST (padrao): paginacao lazy por /query + queryMore
SET sf_force_transport='rest';

-- Bulk API 2.0: paginas de resultado em streaming lazy, com PK chunking opcional
SET sf_force_transport='bulk';

-- auto: sonda a contagem de linhas e entao escolhe rest ou bulk
SET sf_force_transport='auto';
```

- **rest** (padrao) usa o caminho de paginacao REST `/query` + `queryMore`,
  buscado de forma lazy.
- **bulk** usa a Bulk API 2.0. As paginas de resultado vem em streaming
  lazy, com PK chunking opcional (secao 7).
- **auto** executa um `SELECT COUNT()` para sondar a contagem de linhas e
  entao escolhe rest ou bulk com base em `sf_auto_bulk_threshold` (padrao
  50000). A sondagem e controlada por `sf_auto_probe` (padrao true).

Importante sobre `LIMIT` com Bulk: um `LIMIT` no Bulk **nao** e
server-side, entao o job roda por completo. Porem, como as paginas de
resultado vem em streaming lazy, um `LIMIT` pequeno interrompe o download
das paginas seguintes. Note que o **auto nao enxerga o `LIMIT`** ao sondar,
entao para uma consulta com `LIMIT` pequeno contra um objeto enorme, force
`rest` explicitamente:

```sql
SET sf_force_transport='rest';
SELECT Id, Name FROM sf.Account ORDER BY CreatedDate DESC LIMIT 20;
```

### Pushdown de COUNT

`COUNT(*)` e scans de zero colunas executam um `SELECT COUNT()` (sem
paginacao de registros) quando nao ha filtro residual e o scan nao foi
forcado para bulk:

```sql
SELECT COUNT(*) FROM sf.Contact;
```

## 7. Extracoes grandes: Bulk + PK chunking

Para objetos muito grandes, a Bulk API 2.0 mais PK chunking paraleliza a
extracao. `sf_bulk_chunks` (padrao 1 = desligado, limite 8, apenas Bulk)
divide um scan Bulk em N faixas de `Id` disjuntas. A extensao sonda
`MIN(Id)` / `MAX(Id)`, faz uma divisao lexical e executa **um job Bulk por
chunk** em paralelo (ate uma thread por chunk):

```sql
SET sf_force_transport='bulk';
SET sf_bulk_chunks=8;

CREATE TABLE todas_oportunidades AS
SELECT Id, Name, Amount, StageName, CloseDate
FROM sf.Opportunity;
```

Ressalvas: **nao ha ordem global de linhas entre chunks**, e os chunks
podem ser desiguais ou ate vazios, dependendo de como os valores de `Id` se
distribuem.

## 8. Governanca de quota

Para evitar consumir sua cota diaria de API, uma governanca de quota
controla o **inicio de jobs Bulk** (o REST nao e controlado). Antes de
iniciar um job Bulk, a extensao le `GET /limits` e recusa o job quando as
requisicoes restantes estao em ou abaixo de um limite:

```
limite = max(sf_quota_min_remaining, sf_quota_reserve_pct% de DailyApiRequests.Max)
```

Padroes: `sf_quota_min_remaining` e 1000 e `sf_quota_reserve_pct` e 10
(por cento). Configuracoes relevantes:

```sql
SET sf_quota_enabled=true;       -- chave mestra (padrao true)
SET sf_quota_enforce=true;       -- false = apenas avisa (padrao true)
SET sf_quota_fail_open=true;     -- /limits indisponivel -> permite (padrao true)
SET sf_quota_cache_seconds=60;   -- cache do resultado de /limits (padrao 60)
SET sf_quota_min_remaining=1000; -- piso absoluto (padrao 1000)
SET sf_quota_reserve_pct=10;     -- reserva % do maximo diario (padrao 10)
```

Respostas HTTP `429` sao repetidas automaticamente;
`REQUEST_LIMIT_EXCEEDED` e terminal.

## 9. Relacionamentos (opt-in)

Por padrao, colunas de relacionamento ficam desligadas. Defina
`sf_relationships='parent'` para expor lookups de pai de alvo unico como
colunas `STRUCT`:

```sql
SET sf_relationships='parent';

SELECT Account.Name
FROM sf.Contact
LIMIT 10;
```

Escopo e ressalvas:

- A profundidade e 1 (apenas o pai direto).
- Lookups polimorficos sao ignorados.
- Predicados sobre subcampos sao avaliados como filtros residuais (no
  DuckDB).
- O padrao e `'off'`.

## 10. Origem do schema

`sf_schema_source` controla como o schema dos objetos e descoberto:

```sql
-- describe (padrao): describe via REST, autoritativo
SET sf_schema_source='describe';

-- tooling: FieldDefinition da Tooling em lote, rapido
SET sf_schema_source='tooling';
```

- **describe** (padrao) usa a chamada describe do REST e e autoritativo.
- **tooling** usa a API Tooling `FieldDefinition` em lote, rapida, com
  fallback REST por objeto. Ela produz tipos mais grosseiros e reduz o
  pushdown, entao prefira-a apenas quando o describe for lento demais para
  o seu fluxo.

## 11. Diagnosticos

A extensao fornece funcoes de tabela voltadas ao usuario que explicam o que
o ultimo scan fez. A mais completa e `salesforce_query_cost()`:

```sql
SELECT * FROM salesforce_query_cost();
```

Ela retorna, entre outras colunas: `object`, `soql`, `transport`,
`est_rows`, `transport_reason`, `projected_fields`, `total_fields`,
`pushed_filters`, `residual_filters`, `where_pushed`, `pages_fetched`,
`rows_emitted`, `bulk`, `count_pushdown`, `bulk_chunks`,
`quota_remaining`, `quota_allowed` e `guidance`.

Auxiliares focados retornam um unico aspecto do ultimo scan:

```sql
SELECT * FROM salesforce_last_transport();    -- transporte realmente usado
SELECT * FROM salesforce_last_quota();        -- snapshot da quota
SELECT * FROM salesforce_last_soql();         -- SOQL que foi enviado
SELECT * FROM salesforce_last_scan_pages();   -- paginas buscadas
```

Um fluxo tipico e executar sua consulta e entao inspecionar
`salesforce_query_cost()` para confirmar o transporte, os filtros
empurrados vs. residuais e a coluna `guidance` para dicas de ajuste.

## 12. Limitacoes

Conheca estes limites antes de construir sobre a extensao:

- **Somente-leitura.** Toda escrita e DDL contra o catalogo Salesforce
  lanca erro.
- **`LIMIT` no Bulk nao e server-side**; o job Bulk roda por completo (o
  streaming lazy ainda interrompe o download das paginas seguintes com um
  `LIMIT` pequeno).
- **auto nao enxerga o `LIMIT`** ao sondar. Para uma consulta com `LIMIT`
  pequeno em um objeto enorme, force `rest`.
- **Relacionamentos sao apenas de pai, profundidade 1**; lookups
  polimorficos sao ignorados e predicados sobre subcampos sao residuais.
- **A origem de schema tooling produz tipos mais grosseiros** e reduz o
  pushdown.
- **O pushdown de agregacao e apenas para COUNT.**
- **Os PK chunks nao tem ordem global** e podem ser desiguais ou vazios.
- **As plataformas validadas no CI** sao `linux_amd64` e `windows_amd64`;
  macOS, ARM e wasm ainda nao foram validados.
- **Ainda nao esta no catalogo community**, e o projeto e **pre-1.0**.
