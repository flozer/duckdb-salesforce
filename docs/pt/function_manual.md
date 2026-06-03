# Manual de funções da extensão

Referência pública de funções e configurações do `duckdb-salesforce`.

Esta é a contraparte em português de `docs/en/function_manual.md`. Mantenha
os dois arquivos alinhados quando o comportamento público mudar.

## Como ler este manual

Use este documento como referência de comportamento, não como roteiro de
produto. Funções futuras mencionadas no roadmap não estão disponíveis até
serem implementadas, testadas e documentadas aqui.

Este manual cobre toda superfície SQL voltada ao usuário que a extensão
expõe: o tipo de armazenamento de ATTACH `salesforce`, configurações de
sessão, funções de tabela de diagnóstico e funções utilitárias autônomas.
Uma seção final claramente separada documenta os pontos de entrada
exclusivos de depuração/teste, que **não são uma API estável**.

## Requisito de execução

A extensão se comunica com o Salesforce pelas APIs REST e Bulk usando o
fluxo OAuth de refresh token. Você precisa de um Connected App que emita um
`client_id`, um `client_secret` e um `refresh_token` para um usuário com
acesso de leitura aos objetos consultados. Consulte
`docs/CONNECTED_APP.pt-BR.md` para saber como provisionar essas credenciais.

O catálogo anexado é **somente leitura**. A extensão emite consultas SOQL e
lê os resultados; nunca grava de volta no Salesforce.

## Nível 1 - Org anexada (tipo de armazenamento `salesforce`)

### `ATTACH 'salesforce://<org>' AS sf (TYPE salesforce, ...)`

Anexa uma org do Salesforce como um catálogo DuckDB somente leitura. Os
sObjects são expostos como tabelas e resolvidos de forma preguiçosa — um
sObject só é descrito quando é referenciado pela primeira vez, não no
momento do attach.

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT Id, Name, AnnualRevenue
FROM sf.Account
WHERE BillingCountry = 'Brazil';
```

A string `salesforce://<org>` é um rótulo lógico para a org anexada;
`<org>` é um identificador arbitrário escolhido por você (por exemplo
`production` ou `sandbox`). Ela não codifica o host da instância — a URL de
instância real é descoberta na troca do token OAuth.

Parâmetros:

| Parâmetro | Obrigatório | Padrão | Significado |
|---|---|---|---|
| `TYPE salesforce` | sim | — | Seleciona esta extensão de armazenamento |
| `client_id` | sim | — | Consumer key do Connected App |
| `client_secret` | sim | — | Consumer secret do Connected App |
| `refresh_token` | sim | — | Refresh token OAuth do usuário de leitura |
| `login_url` | não | `https://login.salesforce.com` | Host OAuth; use o host de My Domain / sandbox quando aplicável |
| `api_version` | não | padrão da extensão | Versão da API do Salesforce, ex. `60.0` |

Notas de comportamento:

- **Catálogo somente leitura.** Sem INSERT/UPDATE/DELETE/DDL nos sObjects
  anexados.
- **Fluxo OAuth de refresh token.** A extensão troca o refresh token por um
  access token de curta duração e pela URL da instância; renova conforme
  necessário.
- **Resolução preguiçosa de sObject.** O schema de um sObject é buscado no
  primeiro uso, então anexar uma org grande é barato.
- Use `login_url` para apontar para um sandbox
  (`https://test.salesforce.com`) ou um host de login de My Domain.

### `information_schema` via `ATTACH`

Após anexar, use as views de catálogo padrão do DuckDB para inspecionar os
sObjects expostos e suas colunas:

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id 'KEY', client_secret 'SECRET', refresh_token 'TOKEN'
);

SELECT table_name
FROM information_schema.tables
WHERE table_catalog = 'sf';
```

## Nível 2 - Configurações de sessão (`SET ...`)

Estas configurações ajustam a seleção de transporte do scan, o governador
de cota da API, a descoberta de schema, a expansão de relacionamentos e o
chunking do Bulk. Cada uma se aplica à sessão DuckDB atual.

### Seleção de transporte

| Configuração | Tipo | Padrão | Significado |
|---|---|---|---|
| `sf_force_transport` | VARCHAR | `'rest'` | Transporte do scan: `'rest'`, `'bulk'` ou `'auto'` |
| `sf_auto_bulk_threshold` | BIGINT | `50000` | Em `'auto'`: contagem de linhas acima da qual o Bulk é escolhido |
| `sf_auto_probe` | BOOLEAN | `true` | Em `'auto'`: executa a sondagem `COUNT()` para estimar linhas; `false` => usa REST por padrão |

```sql
SET sf_force_transport = 'auto';
SET sf_auto_bulk_threshold = 100000;
```

Quando `sf_force_transport` é `'auto'`, a extensão decide por scan: com
`sf_auto_probe = true` ela executa uma sondagem `COUNT()` e escolhe Bulk
quando a estimativa excede `sf_auto_bulk_threshold`; com
`sf_auto_probe = false` ela pula a sondagem e usa REST por padrão.

### Governador de cota da API

O governador consulta o recurso `/limits` do Salesforce e o limite
`DailyApiRequests` antes de um scan, para evitar esgotar a cota diária de
API da org.

| Configuração | Tipo | Padrão | Significado |
|---|---|---|---|
| `sf_quota_enabled` | BOOLEAN | `true` | Governador ligado; `false` pula totalmente a consulta a `/limits` |
| `sf_quota_enforce` | BOOLEAN | `true` | `false` = apenas aviso (consulta `/limits` mas nunca bloqueia) |
| `sf_quota_fail_open` | BOOLEAN | `true` | Se `/limits` estiver indisponível, permite o scan; `false` = bloqueia |
| `sf_quota_reserve_pct` | BIGINT | `10` | Reserva esta porcentagem de `DailyApiRequests.Max` |
| `sf_quota_min_remaining` | BIGINT | `1000` | Piso absoluto de requisições restantes abaixo do qual scans são bloqueados |
| `sf_quota_cache_seconds` | BIGINT | `60` | TTL em memória de `/limits` por `instance_url` (`0` = sem cache) |

```sql
SET sf_quota_reserve_pct = 20;
SET sf_quota_min_remaining = 5000;
```

O governador bloqueia um scan quando as requisições restantes projetadas
cairiam abaixo da reserva (`sf_quota_reserve_pct` de
`DailyApiRequests.Max`) ou do piso absoluto (`sf_quota_min_remaining`). Com
`sf_quota_enforce = false` ele ainda consulta `/limits` e registra a
decisão, mas nunca bloqueia. Com `sf_quota_fail_open = true` um endpoint
`/limits` indisponível permite o scan; defina como `false` para bloquear
quando o limite não puder ser lido.

### Descoberta de schema e relacionamentos

| Configuração | Tipo | Padrão | Significado |
|---|---|---|---|
| `sf_schema_source` | VARCHAR | `'describe'` | Fonte de descoberta de schema: `'describe'` ou `'tooling'` |
| `sf_relationships` | VARCHAR | `'off'` | `'off'`, ou `'parent'` para expor relacionamentos-pai como colunas STRUCT |

```sql
SET sf_schema_source = 'tooling';
SET sf_relationships = 'parent';
```

`sf_schema_source` seleciona qual API fornece os metadados de campo: a API
Describe padrão (`'describe'`) ou a API Tooling (`'tooling'`). Com
`sf_relationships = 'parent'`, os relacionamentos-pai são expostos como
colunas STRUCT aninhadas ao lado dos campos planos.

### Chunking do Bulk

| Configuração | Tipo | Padrão | Significado |
|---|---|---|---|
| `sf_bulk_chunks` | BIGINT | `1` | Quantidade de chunks de PK para scans Bulk (limite `8`, apenas Bulk) |

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;
```

`sf_bulk_chunks` divide um scan Bulk em N faixas de chave primária que são
buscadas em paralelo (uma thread por chunk). Tem limite de `8` e se aplica
apenas quando o transporte Bulk é usado; não tem efeito em scans REST.

## Nível 3 - Diagnóstico e observabilidade

Estas são funções de tabela sem argumentos que reportam sobre o **último
scan** da sessão atual. São de melhor esforço e refletem um instantâneo de
thread única; sob execução Bulk paralela, descrevem a visão da thread
coordenadora.

### `salesforce_query_cost()`

Retorna uma linha descrevendo o planejamento e a execução do scan mais
recente: transporte escolhido e o porquê, contagens de pushdown de
projeção/filtro, paginação, decisão de cota e uma string de orientação
legível por humanos.

```sql
SELECT * FROM sf.Account WHERE BillingCountry = 'Brazil';
SELECT * FROM salesforce_query_cost();
```

Colunas de saída:

| Coluna | Tipo | Notas |
|---|---|---|
| `object` | VARCHAR | sObject consultado |
| `soql` | VARCHAR | SOQL enviado ao Salesforce |
| `transport` | VARCHAR | `rest` ou `bulk` |
| `est_rows` | BIGINT | Contagem estimada de linhas usada no planejamento |
| `transport_reason` | VARCHAR | Por que este transporte foi escolhido |
| `projected_fields` | BIGINT | Campos requisitados no SELECT |
| `total_fields` | BIGINT | Total de campos no sObject |
| `pushed_filters` | BIGINT | Predicados empurrados para o SOQL |
| `residual_filters` | BIGINT | Predicados mantidos e avaliados no DuckDB |
| `where_pushed` | VARCHAR | A cláusula WHERE efetivamente empurrada |
| `pages_fetched` | BIGINT | Páginas de resultado da API buscadas |
| `rows_emitted` | BIGINT | Linhas retornadas ao DuckDB |
| `bulk` | BOOLEAN | Se o transporte Bulk foi usado |
| `count_pushdown` | BOOLEAN | Se um pushdown de `COUNT()` foi usado |
| `bulk_chunks` | BIGINT | Quantidade de chunks de PK aplicada (Bulk) |
| `quota_remaining` | BIGINT | Requisições de API restantes no momento da decisão |
| `quota_allowed` | BOOLEAN | Se o governador de cota permitiu o scan |
| `guidance` | VARCHAR | Conselho legível por humanos para ajustar o scan |

Instantâneo do último scan, de melhor esforço, thread única.

### `salesforce_last_transport()`

Retorna a decisão de transporte do último scan.

| Coluna | Tipo | Notas |
|---|---|---|
| `transport` | VARCHAR | `rest` ou `bulk` |
| `est_rows` | BIGINT | Linhas estimadas usadas na decisão |
| `reason` | VARCHAR | Por que este transporte foi escolhido |

### `salesforce_last_quota()`

Retorna a decisão do governador de cota do último scan.

| Coluna | Tipo | Notas |
|---|---|---|
| `limit_name` | VARCHAR | O limite do Salesforce consultado (ex. `DailyApiRequests`) |
| `max` | BIGINT | O máximo do limite |
| `remaining` | BIGINT | Requisições restantes no momento da decisão |
| `threshold` | BIGINT | Limiar efetivo de bloqueio (reserva / piso) |
| `allowed` | BOOLEAN | Se o scan foi permitido |
| `reason` | VARCHAR | Explicação da decisão |

### `salesforce_last_soql()`

Retorna a string SOQL enviada no último scan.

| Coluna | Tipo | Notas |
|---|---|---|
| `soql` | VARCHAR | A string de consulta SOQL |

### `salesforce_last_scan_pages()`

Retorna a quantidade de páginas de resultado da API buscadas durante o
último scan.

| Coluna | Tipo | Notas |
|---|---|---|
| `pages` | BIGINT | Páginas de resultado da API buscadas |

## Nível 4 - Funções utilitárias / autônomas

Estas funções recebem as credenciais como argumentos nomeados e **não**
exigem um `ATTACH`. São úteis para inspeção pontual de schema ou consultas
brutas.

### `salesforce_describe(object, client_id := ..., ...)`

Descreve o schema de um único sObject sem anexar a org. Retorna uma linha
por campo.

```sql
SELECT *
FROM salesforce_describe(
  'Account',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

Argumentos:

| Argumento | Obrigatório | Significado |
|---|---|---|
| `object` | sim | Nome de API do sObject a descrever (posicional) |
| `client_id :=` | sim | Consumer key do Connected App |
| `client_secret :=` | sim | Consumer secret do Connected App |
| `refresh_token :=` | sim | Refresh token OAuth |
| `login_url :=` | não | Host OAuth (padrão `https://login.salesforce.com`) |
| `api_version :=` | não | Versão da API do Salesforce |

Colunas de saída (nomes aproximados):

| Coluna | Tipo | Notas |
|---|---|---|
| `name` | VARCHAR | Nome de API do campo |
| `sf_type` | VARCHAR | Tipo de campo do Salesforce |
| `duckdb_type` | VARCHAR | Tipo DuckDB mapeado |
| `nillable` | BOOLEAN | Se o campo aceita NULL |
| `length` | BIGINT | Comprimento declarado (campos de texto) |
| `precision` | BIGINT | Precisão numérica |
| `scale` | BIGINT | Escala numérica |
| `filterable` | BOOLEAN | Se o campo pode aparecer em um WHERE de SOQL |
| `sortable` | BOOLEAN | Se o campo pode aparecer em ORDER BY |

Use isto para inspecionar o schema de um sObject sem anexar o catálogo
inteiro da org.

### `salesforce_query(soql, client_id := ..., ...)`

Executa uma consulta SOQL bruta e retorna os registros de resultado como
strings JSON de registro brutas, uma por linha. É um utilitário de baixo
nível; nenhum mapeamento de schema ou planejamento de pushdown é aplicado.

```sql
SELECT *
FROM salesforce_query(
  'SELECT Id, Name FROM Account LIMIT 10',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

Os argumentos espelham os de `salesforce_describe` (o primeiro argumento
posicional é a string SOQL em vez do nome do objeto; os mesmos argumentos
nomeados de credencial se aplicam).

## Referência de pushdown

O planejador de scan empurra para o SOQL o máximo da consulta que for
seguro, e mantém o restante como um filtro **residual** avaliado no DuckDB.
Um filtro residual nunca altera a correção — significa apenas que mais
linhas são buscadas do que o estritamente necessário.

Empurrado para o SOQL:

- **Projeção** — apenas os campos referenciados são requisitados.
- **Comparações** — `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Testes de nulo** — `IS NULL`, `IS NOT NULL`.
- **`AND`** de predicados empurráveis.

Empurrado como pré-filtro superconjunto e depois mantido como residual (o
DuckDB reavalia exatamente):

- **`IN`** — empurrado como pré-filtro superconjunto, mantido residual.
- **`LIKE`** prefixo / sufixo / contém — empurrado como superconjunto,
  mantido residual.
- **`OR`** — empurrado apenas quando todos os filhos são, eles mesmos,
  seguros de empurrar; empurrado como superconjunto, mantido residual.

Mantido totalmente residual (não empurrado):

- Chamadas de função em predicados.
- `NOT`.
- Predicados em campos não filtráveis.
- Uma cláusula WHERE que excederia 4000 caracteres ao ser renderizada como
  SOQL.

Agregações:

- `COUNT(*)` é empurrado como um `COUNT()` de SOQL.
- Agregações diferentes de `COUNT(*)` **não** são empurradas.

Use `salesforce_query_cost()` (colunas `pushed_filters`, `residual_filters`,
`where_pushed`, `count_pushdown`) para ver exatamente o que chegou ao
Salesforce em um determinado scan.

## Funções de depuração / exclusivas de teste

> **Não é uma API estável.** Tudo nesta seção existe para a própria suíte
> de testes da extensão e para depuração de baixo nível. Nomes, argumentos,
> formatos de saída e a própria existência podem mudar sem aviso. Não
> construa fluxos de produção sobre estas funções.

- `salesforce_describe_calls()` — instrumentação de contagem de chamadas à
  API Describe.
- `salesforce_global_describe_calls()` — instrumentação de contagem de
  chamadas à API Global Describe.
- `salesforce_tooling_calls()` — instrumentação de contagem de chamadas à
  API Tooling.
- `salesforce_last_bulk_create_body()` — o corpo da requisição da chamada
  mais recente de criação de job Bulk.
- `salesforce_decode(fields_json, records_json)` — decodifica JSON bruto de
  campos/registros do Salesforce em linhas tipadas; usado para testar o
  caminho de decodificação isoladamente.
- `sf_url_encode(s)` — codifica uma string em URL; usado para testar a
  construção de query string.

### Configurações de mock (suíte de testes offline)

Existe uma família de configurações `sf_mock_*` para direcionar a extensão
contra fixtures gravadas na suíte de testes offline (sem org ao vivo). Elas
**não são para uso em produção** e são intencionalmente não documentadas
como superfície estável; existem apenas para que a infraestrutura de testes
possa injetar respostas de API enlatadas.

## Premissa de documentação

Quando o comportamento público mudar:

- atualize este arquivo
- atualize `docs/en/function_manual.md`
- atualize os guias de uso quando exemplos ou fluxos mudarem
- atualize os arquivos de roadmap quando o status mudar

Os documentos PT/EN devem permanecer alinhados em significado, status,
ressalvas e exemplos.
