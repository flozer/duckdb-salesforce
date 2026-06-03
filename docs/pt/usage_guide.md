# duckdb-salesforce - Guia de uso para analistas

Este guia mostra como consultar uma org Salesforce diretamente do DuckDB
usando a extensão `duckdb-salesforce`. A extensão anexa sua org como um
**catálogo somente-leitura**: objetos Salesforce (sObjects) aparecem como
tabelas das quais você faz `SELECT` com SQL comum, enquanto a extensão
traduz sua consulta em SOQL e escolhe a API correta do Salesforce (REST ou
Bulk) nos bastidores.

É prático e baseado em exemplos. Cada configuração e função citada aqui é
real; nada foi inventado.

## Referências oficiais DuckDB

Este guia segue conceitos documentados pelo DuckDB:

- [`ATTACH`](https://duckdb.org/docs/current/sql/statements/attach.html):
  anexa outro catálogo ao DuckDB. Esta extensão usa o mesmo modelo para
  expor uma org Salesforce como catálogo somente-leitura.
- [`SELECT`](https://duckdb.org/docs/stable/sql/statements/select):
  o único comando que você executa contra o catálogo Salesforce. Toda
  escrita e DDL lança erro.
- [`CREATE TABLE`](https://duckdb.org/docs/stable/sql/statements/create_table):
  `CREATE TABLE ... AS SELECT` materializa uma consulta Salesforce em uma
  tabela local do DuckDB.
- [Table functions](https://duckdb.org/docs/stable/sql/functions/overview):
  esta extensão fornece funções de diagnóstico como
  `salesforce_query_cost()`.

## 1. Conceitos antes de começar

### Catálogo somente-leitura

`ATTACH` expõe a org como catálogo. Você só pode ler dele: todo `INSERT`,
`UPDATE`, `DELETE` e DDL contra o catálogo Salesforce lança erro. Se você
precisa de uma cópia local gravável, materialize uma consulta em uma tabela
DuckDB (veja a seção 5).

### Tabelas são sObjects

Cada tabela no catálogo anexado é um sObject do Salesforce (`Account`,
`Contact`, `Opportunity`, objetos personalizados terminados em `__c`, e
assim por diante). O schema é resolvido **sob demanda** (lazy) na
primeira vez que você referencia um objeto, então anexar é barato e rápido.

### Autenticação e segurança

A autenticação é **OAuth 2.0 apenas por refresh-token**. Você fornece um
`client_id`, um `client_secret` e um `refresh_token`. As credenciais ficam
somente em memória e nunca são registradas em log. A verificação de
certificado TLS está sempre ativa.

## 2. Instalação e carregamento

Um build local não é **assinado**, então o DuckDB não o carrega sem que
você permita explicitamente:

```sql
SET allow_unsigned_extensions=true;
LOAD 'caminho/para/salesforce.duckdb_extension';
```

Para os passos de build e instalação por plataforma, veja
[docs/en/guide_windows.md](../en/guide_windows.md) (Windows) e
[docs/en/guide_linux.md](../en/guide_linux.md) (Linux).

As plataformas validadas no CI hoje são `linux_amd64` e `windows_amd64`. A
extensão ainda não foi publicada no catálogo community, então ainda não
existe um `INSTALL ... FROM community`.

## 3. Conectar ao Salesforce

Anexe a org com o tipo de catálogo `salesforce` e suas credenciais OAuth:

```sql
ATTACH 'salesforce://<org>' AS sf (
    TYPE salesforce,
    client_id 'sua_consumer_key',
    client_secret 'seu_consumer_secret',
    refresh_token 'seu_refresh_token'
);
```

Dois parâmetros opcionais cobrem sandboxes e versões de API fixadas:

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
[docs/CONNECTED_APP.pt-BR.md](../CONNECTED_APP.pt-BR.md).

## 4. Primeiras consultas

Uma vez anexada, consulte sObjects como qualquer outra tabela:

```sql
SELECT Id, Name
FROM sf.Account
WHERE Name = 'Acme'
LIMIT 10;
```

O schema é resolvido sob demanda (lazy) na primeira referência. Para listar
os objetos que você pode consultar, execute um único describe global via:

```sql
SHOW TABLES;
-- ou
SELECT * FROM duckdb_tables();
```

## 5. Materializar resultados localmente

Para manter um snapshot local e consultável (e fugir das limitações de
somente-leitura), copie uma consulta Salesforce para uma tabela DuckDB:

```sql
CREATE TABLE contas_locais AS
SELECT Id, Name, Industry, AnnualRevenue
FROM sf.Account
WHERE Industry = 'Manufacturing';
```

A partir daí você pode consultar, juntar e agregar `contas_locais` sem
novas chamadas de API.

## 6. Transportes: REST, Bulk e auto

A extensão pode buscar linhas por diferentes APIs do Salesforce. Escolha
com `sf_force_transport`:

```sql
-- REST (padrão): paginação lazy por /query + queryMore
SET sf_force_transport='rest';

-- Bulk API 2.0: páginas de resultado em streaming lazy, com PK chunking opcional
SET sf_force_transport='bulk';

-- auto: sonda a contagem de linhas e então escolhe rest ou bulk
SET sf_force_transport='auto';
```

- **rest** (padrão) usa o caminho de paginação REST `/query` + `queryMore`,
  buscado de forma lazy.
- **bulk** usa a Bulk API 2.0. As páginas de resultado vêm em streaming
  lazy, com PK chunking opcional (seção 7).
- **auto** executa um `SELECT COUNT()` para sondar a contagem de linhas e
  então escolhe rest ou bulk com base em `sf_auto_bulk_threshold` (padrão
  50000). A sondagem é controlada por `sf_auto_probe` (padrão true).

Importante sobre `LIMIT` com Bulk: um `LIMIT` no Bulk **não** é
server-side, então o job roda por completo. Porém, como as páginas de
resultado vêm em streaming lazy, um `LIMIT` pequeno interrompe o download
das páginas seguintes. Note que o **auto não enxerga o `LIMIT`** ao sondar,
então para uma consulta com `LIMIT` pequeno contra um objeto enorme, force
`rest` explicitamente:

```sql
SET sf_force_transport='rest';
SELECT Id, Name FROM sf.Account ORDER BY CreatedDate DESC LIMIT 20;
```

### Pushdown de COUNT

`COUNT(*)` e scans de zero colunas executam um `SELECT COUNT()` (sem
paginação de registros) quando não há filtro residual e o scan não foi
forçado para bulk:

```sql
SELECT COUNT(*) FROM sf.Contact;
```

## 7. Extrações grandes: Bulk + PK chunking

Para objetos muito grandes, a Bulk API 2.0 mais PK chunking paraleliza a
extração. `sf_bulk_chunks` (padrão 1 = desligado, limite 8, apenas Bulk)
divide um scan Bulk em N faixas de `Id` disjuntas. A extensão sonda
`MIN(Id)` / `MAX(Id)`, faz uma divisão lexical e executa **um job Bulk por
chunk** em paralelo (até uma thread por chunk):

```sql
SET sf_force_transport='bulk';
SET sf_bulk_chunks=8;

CREATE TABLE todas_oportunidades AS
SELECT Id, Name, Amount, StageName, CloseDate
FROM sf.Opportunity;
```

Ressalvas: **não há ordem global de linhas entre chunks**, e os chunks
podem ser desiguais ou até vazios, dependendo de como os valores de `Id` se
distribuem.

## 8. Governança de quota

Para evitar consumir sua cota diária de API, uma governança de quota
controla o **início de jobs Bulk** (o REST não é controlado). Antes de
iniciar um job Bulk, a extensão lê `GET /limits` e recusa o job quando as
requisições restantes estão em ou abaixo de um limite:

```
limite = max(sf_quota_min_remaining, sf_quota_reserve_pct% de DailyApiRequests.Max)
```

Padrões: `sf_quota_min_remaining` é 1000 e `sf_quota_reserve_pct` é 10
(por cento). Configurações relevantes:

```sql
SET sf_quota_enabled=true;       -- chave mestra (padrão true)
SET sf_quota_enforce=true;       -- false = apenas avisa (padrão true)
SET sf_quota_fail_open=true;     -- /limits indisponível -> permite (padrão true)
SET sf_quota_cache_seconds=60;   -- cache do resultado de /limits (padrão 60)
SET sf_quota_min_remaining=1000; -- piso absoluto (padrão 1000)
SET sf_quota_reserve_pct=10;     -- reserva % do máximo diário (padrão 10)
```

Respostas HTTP `429` são repetidas automaticamente;
`REQUEST_LIMIT_EXCEEDED` é terminal.

## 9. Relacionamentos (opt-in)

Por padrão, colunas de relacionamento ficam desligadas. Defina
`sf_relationships='parent'` para expor lookups de pai de alvo único como
colunas `STRUCT`:

```sql
SET sf_relationships='parent';

SELECT Account.Name
FROM sf.Contact
LIMIT 10;
```

Escopo e ressalvas:

- A profundidade é 1 (apenas o pai direto).
- Lookups polimórficos são ignorados.
- Predicados sobre subcampos são avaliados como filtros residuais (no
  DuckDB).
- O padrão é `'off'`.

## 10. Origem do schema

`sf_schema_source` controla como o schema dos objetos é descoberto:

```sql
-- describe (padrão): describe via REST, autoritativo
SET sf_schema_source='describe';

-- tooling: FieldDefinition da Tooling em lote, rápido
SET sf_schema_source='tooling';
```

- **describe** (padrão) usa a chamada describe do REST e é autoritativo.
- **tooling** usa a API Tooling `FieldDefinition` em lote, rápida, com
  fallback REST por objeto. Ela produz tipos mais grosseiros e reduz o
  pushdown, então prefira-a apenas quando o describe for lento demais para
  o seu fluxo.

## 11. Diagnósticos

A extensão fornece funções de tabela voltadas ao usuário que explicam o que
o último scan fez. A mais completa é `salesforce_query_cost()`:

```sql
SELECT * FROM salesforce_query_cost();
```

Ela retorna, entre outras colunas: `object`, `soql`, `transport`,
`est_rows`, `transport_reason`, `projected_fields`, `total_fields`,
`pushed_filters`, `residual_filters`, `where_pushed`, `pages_fetched`,
`rows_emitted`, `bulk`, `count_pushdown`, `bulk_chunks`,
`quota_remaining`, `quota_allowed` e `guidance`.

Auxiliares focados retornam um único aspecto do último scan:

```sql
SELECT * FROM salesforce_last_transport();    -- transporte realmente usado
SELECT * FROM salesforce_last_quota();        -- snapshot da quota
SELECT * FROM salesforce_last_soql();         -- SOQL que foi enviado
SELECT * FROM salesforce_last_scan_pages();   -- páginas buscadas
```

Um fluxo típico é executar sua consulta e então inspecionar
`salesforce_query_cost()` para confirmar o transporte, os filtros com
pushdown vs. residuais e a coluna `guidance` para dicas de ajuste.

## 12. Limitações

Conheça estes limites antes de construir sobre a extensão:

- **Somente-leitura.** Toda escrita e DDL contra o catálogo Salesforce
  lança erro.
- **`LIMIT` no Bulk não é server-side**; o job Bulk roda por completo (o
  streaming lazy ainda interrompe o download das páginas seguintes com um
  `LIMIT` pequeno).
- **auto não enxerga o `LIMIT`** ao sondar. Para uma consulta com `LIMIT`
  pequeno em um objeto enorme, force `rest`.
- **Relacionamentos são apenas de pai, profundidade 1**; lookups
  polimórficos são ignorados e predicados sobre subcampos são residuais.
- **A origem de schema tooling produz tipos mais grosseiros** e reduz o
  pushdown.
- **O pushdown de agregação é apenas para COUNT.**
- **Os PK chunks não têm ordem global** e podem ser desiguais ou vazios.
- **As plataformas validadas no CI** são `linux_amd64` e `windows_amd64`;
  macOS, ARM e wasm ainda não foram validados.
- **Ainda não está no catálogo community**, e o projeto é **pré-1.0**.
