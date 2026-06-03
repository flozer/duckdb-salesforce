# Manual de funções da extensão

Referência pública de funções e configurações do `duckdb-salesforce`.

Esta é a contraparte em português de `docs/en/function_manual.md`. Mantenha
os dois arquivos alinhados quando o comportamento público mudar.

## Como ler este manual

Este manual é um guia prático para quem está começando. Para cada função e
configuração importante usamos uma estrutura fixa de quatro partes:

- **O que faz** — uma frase direta sobre o resultado.
- **Como funciona** — a mecânica por trás, e (para diagnósticos) a tabela de
  colunas de saída; para configurações, o tipo, o padrão e os valores
  aceitos.
- **Para que serve** — quando e por que você usaria isso no seu trabalho.
- **Uso no dia a dia** — um exemplo SQL simples e executável.

Use este documento como referência de comportamento, não como roteiro de
produto. Funções futuras mencionadas no roadmap não estão disponíveis até
serem implementadas, testadas e documentadas aqui.

O manual cobre toda a superfície SQL voltada ao usuário que a extensão
expõe: o tipo de armazenamento de `ATTACH` `salesforce`, as configurações
de sessão, as funções de tabela de diagnóstico e as funções utilitárias
autônomas. Uma seção final, claramente separada, documenta os pontos de
entrada exclusivos de depuração/teste, que **não são uma API estável**.

Nos exemplos, assumimos que você já executou um `ATTACH ... AS sf` e que os
sObjects `Account` e `Contact` estão acessíveis como `sf.Account` e
`sf.Contact`.

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

#### O que faz

Anexa uma org do Salesforce como um catálogo DuckDB somente leitura, no
qual cada sObject aparece como uma tabela que você consulta com SQL normal.

#### Como funciona

Os sObjects são resolvidos sob demanda (lazy): o schema de um sObject só é
buscado quando ele é referenciado pela primeira vez, e não no momento do
`ATTACH`. Por isso, anexar uma org grande é barato.

A string `salesforce://<org>` é apenas um rótulo lógico para a org anexada;
`<org>` é um identificador arbitrário escolhido por você (por exemplo
`production` ou `sandbox`). Ela não codifica o host da instância — a URL de
instância real é descoberta durante a troca do token OAuth.

A extensão troca o refresh token por um access token de curta duração e pela
URL da instância, e renova esse access token conforme necessário.

Parâmetros do `ATTACH`:

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
- **Lazy sObject resolution.** O schema de um sObject é buscado no primeiro
  uso, então anexar uma org grande é barato.
- Use `login_url` para apontar para um sandbox
  (`https://test.salesforce.com`) ou um host de login de My Domain.

#### Para que serve

É o ponto de partida de quase tudo: depois do `ATTACH`, você consulta o
Salesforce como se fosse um banco local, podendo fazer joins, filtros e
agregações em SQL sem escrever SOQL à mão.

#### Uso no dia a dia

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT Id, Name, AnnualRevenue
FROM sf.Account
WHERE Industry = 'Technology';
```

### `information_schema` via `ATTACH`

#### O que faz

Permite listar os sObjects expostos e suas colunas usando as views de
catálogo padrão do DuckDB.

#### Como funciona

Depois de anexar, o catálogo `sf` participa das views normais de
`information_schema` do DuckDB. Você filtra por `table_catalog = 'sf'` para
ver apenas os objetos da org anexada. Lembre-se de que a resolução é lazy:
um sObject pode só ter o schema completo materializado depois de ser
referenciado pela primeira vez.

#### Para que serve

Útil para descobrir quais tabelas e colunas estão disponíveis antes de
escrever uma consulta, direto pelo SQL, sem sair do DuckDB.

#### Uso no dia a dia

```sql
SELECT table_name
FROM information_schema.tables
WHERE table_catalog = 'sf';
```

## Nível 2 - Configurações de sessão (`SET ...`)

Estas configurações ajustam a seleção de transporte do scan, o governador
de cota da API, a descoberta de schema, a expansão de relacionamentos e o
chunking do Bulk. Cada uma se aplica à sessão DuckDB atual.

### `sf_force_transport`

#### O que faz

Escolhe qual transporte a extensão usa para ler um sObject: REST, Bulk API
ou decisão automática.

#### Como funciona

- Tipo: `VARCHAR`
- Padrão: `'rest'`
- Valores aceitos: `'rest'`, `'bulk'` ou `'auto'`

Com `'rest'`, todos os scans usam a API REST. Com `'bulk'`, usam a Bulk API.
Com `'auto'`, a extensão decide por scan (veja `sf_auto_bulk_threshold` e
`sf_auto_probe`).

#### Para que serve

REST é ideal para consultas pequenas e interativas; Bulk é melhor para
extrair grandes volumes. Use `'auto'` quando não quiser decidir manualmente.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'auto';
SELECT Id, Name FROM sf.Account;
```

### `sf_auto_bulk_threshold`

#### O que faz

Define o ponto de corte de linhas em que o modo `'auto'` passa a preferir o
Bulk em vez do REST.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `50000`

Só tem efeito quando `sf_force_transport = 'auto'`. Se a contagem estimada
de linhas exceder este valor, a extensão escolhe Bulk.

#### Para que serve

Ajusta a fronteira entre "consulta interativa" e "extração em massa" de
acordo com a sua org, sem ter que forçar o transporte manualmente.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'auto';
SET sf_auto_bulk_threshold = 100000;
SELECT Id, Name FROM sf.Account;
```

### `sf_auto_probe`

#### O que faz

Controla se o modo `'auto'` executa uma sondagem `COUNT()` para estimar
quantas linhas o scan vai retornar.

#### Como funciona

- Tipo: `BOOLEAN`
- Padrão: `true`

Com `true`, a extensão roda um `COUNT()` antes do scan e compara o resultado
com `sf_auto_bulk_threshold`. Com `false`, ela pula essa sondagem e usa REST
por padrão.

#### Para que serve

Desligar a sondagem evita uma chamada extra à API quando você prefere que o
`'auto'` seja conservador e fique no REST por padrão.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'auto';
SET sf_auto_probe = false;
SELECT Id, Name FROM sf.Account;
```

### `sf_bulk_chunks`

#### O que faz

Divide um scan Bulk em N faixas de chave primária (PK chunking) buscadas em
paralelo.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `1`
- Limite máximo: `8`

Cada chunk é uma faixa de PK buscada em sua própria thread (uma thread por
chunk). Aplica-se **apenas** quando o transporte Bulk é usado; não tem
efeito em scans REST.

#### Para que serve

Acelera extrações grandes via Bulk ao paralelizar a leitura de faixas de
chave primária.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_chunks = 4;
SELECT Id, Name FROM sf.Account;
```

### Governador de cota da API

O governador consulta o recurso `/limits` do Salesforce e o limite
`DailyApiRequests` antes de um scan, para evitar esgotar a cota diária de
API da org. Ele bloqueia um scan quando as requisições restantes projetadas
cairiam abaixo da reserva (`sf_quota_reserve_pct` de `DailyApiRequests.Max`)
ou do piso absoluto (`sf_quota_min_remaining`).

### `sf_quota_enabled`

#### O que faz

Liga ou desliga totalmente o governador de cota.

#### Como funciona

- Tipo: `BOOLEAN`
- Padrão: `true`

Com `false`, a extensão pula completamente a consulta a `/limits` — nenhuma
verificação de cota é feita.

#### Para que serve

Desligar é útil em ambientes de teste ou quando você já controla a cota por
fora e quer eliminar a chamada extra a `/limits`.

#### Uso no dia a dia

```sql
SET sf_quota_enabled = false;
SELECT Id, Name FROM sf.Account;
```

### `sf_quota_enforce`

#### O que faz

Decide se o governador apenas avisa ou realmente bloqueia scans.

#### Como funciona

- Tipo: `BOOLEAN`
- Padrão: `true`

Com `false`, a extensão ainda consulta `/limits` e registra a decisão, mas
nunca bloqueia o scan (modo apenas-aviso).

#### Para que serve

O modo apenas-aviso permite observar quando você se aproximaria do limite
sem interromper o trabalho.

#### Uso no dia a dia

```sql
SET sf_quota_enforce = false;
SELECT Id, Name FROM sf.Account;
```

### `sf_quota_fail_open`

#### O que faz

Define o que acontece quando o endpoint `/limits` não pode ser lido.

#### Como funciona

- Tipo: `BOOLEAN`
- Padrão: `true`

Com `true`, um `/limits` indisponível permite o scan mesmo assim. Com
`false`, um `/limits` indisponível bloqueia o scan.

#### Para que serve

Use `false` quando precisar de garantia de que nenhum scan rode sem que a
cota tenha sido efetivamente verificada.

#### Uso no dia a dia

```sql
SET sf_quota_fail_open = false;
SELECT Id, Name FROM sf.Account;
```

### `sf_quota_reserve_pct`

#### O que faz

Reserva uma porcentagem do total diário de requisições de API que o
governador nunca deixa ser consumida por scans.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `10`

Reserva esta porcentagem de `DailyApiRequests.Max`. Se um scan fizesse as
requisições restantes caírem abaixo dessa reserva, ele é bloqueado.

#### Para que serve

Garante que outras integrações da org tenham folga de cota, deixando uma
margem de segurança para o resto do dia.

#### Uso no dia a dia

```sql
SET sf_quota_reserve_pct = 20;
SELECT Id, Name FROM sf.Account;
```

### `sf_quota_min_remaining`

#### O que faz

Define um piso absoluto de requisições restantes abaixo do qual nenhum scan
é permitido.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `1000`

Independentemente da porcentagem de reserva, se as requisições restantes
ficarem abaixo deste valor, o scan é bloqueado.

#### Para que serve

É uma proteção em números absolutos, útil para orgs em que uma porcentagem
sozinha não daria uma margem suficiente.

#### Uso no dia a dia

```sql
SET sf_quota_min_remaining = 5000;
SELECT Id, Name FROM sf.Account;
```

### `sf_quota_cache_seconds`

#### O que faz

Define por quanto tempo a resposta de `/limits` fica em cache na memória.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `60`

É o TTL em memória de `/limits` por `instance_url`. Com `0`, não há cache e
cada decisão de cota consulta `/limits` novamente.

#### Para que serve

Um cache reduz chamadas repetidas a `/limits` quando você roda muitos scans
em sequência; zere o valor quando precisar de leituras sempre frescas.

#### Uso no dia a dia

```sql
SET sf_quota_cache_seconds = 0;
SELECT Id, Name FROM sf.Account;
```

### `sf_schema_source`

#### O que faz

Escolhe qual API fornece os metadados de campo dos sObjects.

#### Como funciona

- Tipo: `VARCHAR`
- Padrão: `'describe'`
- Valores aceitos: `'describe'` ou `'tooling'`

Com `'describe'`, o schema vem da API Describe padrão. Com `'tooling'`, vem
da Tooling API.

#### Para que serve

Algumas informações de metadados ficam disponíveis pela Tooling API; troque
a fonte quando precisar do detalhamento que ela oferece.

#### Uso no dia a dia

```sql
SET sf_schema_source = 'tooling';
SELECT Id, Name FROM sf.Account;
```

### `sf_relationships`

#### O que faz

Controla se relacionamentos-pai (parent relationship) são expostos como
colunas STRUCT.

#### Como funciona

- Tipo: `VARCHAR`
- Padrão: `'off'`
- Valores aceitos: `'off'` ou `'parent'`

Com `'parent'`, os relacionamentos-pai aparecem como colunas STRUCT
aninhadas ao lado dos campos planos do sObject.

#### Para que serve

Facilita ler campos do registro-pai (por exemplo, o `Account` de um
`Contact`) sem fazer joins manuais.

#### Uso no dia a dia

```sql
SET sf_relationships = 'parent';
SELECT Id, Email FROM sf.Contact;
```

## Nível 3 - Diagnóstico e observabilidade

Estas são funções de tabela sem argumentos que reportam sobre o **último
scan** da sessão atual. São de melhor esforço e refletem um instantâneo de
thread única; sob execução Bulk paralela, descrevem a visão da thread
coordenadora.

### `salesforce_query_cost()`

#### O que faz

Retorna uma linha resumindo como o último scan foi planejado e executado:
transporte escolhido e o porquê, pushdown de projeção/filtro, paginação,
decisão de cota e uma orientação legível por humanos.

#### Como funciona

Lê o instantâneo do último scan da sessão atual (melhor esforço, thread
única) e o devolve com estas colunas de saída:

| Coluna | Tipo | Notas |
|---|---|---|
| `object` | VARCHAR | sObject consultado |
| `soql` | VARCHAR | SOQL enviado ao Salesforce |
| `transport` | VARCHAR | `rest` ou `bulk` |
| `est_rows` | BIGINT | Contagem estimada de linhas usada no planejamento |
| `transport_reason` | VARCHAR | Por que este transporte foi escolhido |
| `projected_fields` | BIGINT | Campos requisitados no SELECT |
| `total_fields` | BIGINT | Total de campos no sObject |
| `pushed_filters` | BIGINT | Predicados com pushdown para o SOQL |
| `residual_filters` | BIGINT | Predicados mantidos e avaliados no DuckDB |
| `where_pushed` | VARCHAR | A cláusula WHERE efetivamente enviada via pushdown |
| `pages_fetched` | BIGINT | Páginas de resultado da API buscadas |
| `rows_emitted` | BIGINT | Linhas retornadas ao DuckDB |
| `bulk` | BOOLEAN | Se o transporte Bulk foi usado |
| `count_pushdown` | BOOLEAN | Se um pushdown de `COUNT()` foi usado |
| `bulk_chunks` | BIGINT | Quantidade de chunks de PK aplicada (Bulk) |
| `quota_remaining` | BIGINT | Requisições de API restantes no momento da decisão |
| `quota_allowed` | BOOLEAN | Se o governador de cota permitiu o scan |
| `guidance` | VARCHAR | Conselho legível por humanos para ajustar o scan |

#### Para que serve

É a primeira parada quando uma consulta está lenta ou cara: mostra o que
realmente chegou ao Salesforce e sugere como ajustar o scan.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account WHERE Industry = 'Technology';
SELECT * FROM salesforce_query_cost();
```

### `salesforce_last_soql()`

#### O que faz

Retorna exatamente a string SOQL que foi enviada no último scan.

#### Como funciona

Devolve uma única linha com o SOQL do último scan da sessão:

| Coluna | Tipo | Notas |
|---|---|---|
| `soql` | VARCHAR | A string de consulta SOQL |

#### Para que serve

Útil para confirmar como o SQL do DuckDB foi traduzido em SOQL — em
especial quais campos e qual `WHERE` foram realmente enviados.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account WHERE Industry = 'Technology';
SELECT * FROM salesforce_last_soql();
```

### `salesforce_last_transport()`

#### O que faz

Retorna a decisão de transporte do último scan.

#### Como funciona

Devolve uma linha descrevendo o transporte escolhido e a estimativa que
levou à decisão:

| Coluna | Tipo | Notas |
|---|---|---|
| `transport` | VARCHAR | `rest` ou `bulk` |
| `est_rows` | BIGINT | Linhas estimadas usadas na decisão |
| `reason` | VARCHAR | Por que este transporte foi escolhido |

#### Para que serve

Permite confirmar se o modo `'auto'` escolheu REST ou Bulk e entender o
motivo, sem ler todas as colunas de `salesforce_query_cost()`.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_transport();
```

### `salesforce_last_quota()`

#### O que faz

Retorna a decisão do governador de cota referente ao último scan.

#### Como funciona

Devolve uma linha com o limite consultado e o resultado da verificação:

| Coluna | Tipo | Notas |
|---|---|---|
| `limit_name` | VARCHAR | O limite do Salesforce consultado (ex. `DailyApiRequests`) |
| `max` | BIGINT | O máximo do limite |
| `remaining` | BIGINT | Requisições restantes no momento da decisão |
| `threshold` | BIGINT | Limiar efetivo de bloqueio (reserva / piso) |
| `allowed` | BOOLEAN | Se o scan foi permitido |
| `reason` | VARCHAR | Explicação da decisão |

#### Para que serve

Mostra quanto da cota diária ainda resta e por que o governador permitiu ou
bloquearia um scan — útil para calibrar `sf_quota_reserve_pct` e
`sf_quota_min_remaining`.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_quota();
```

### `salesforce_last_scan_pages()`

#### O que faz

Retorna quantas páginas de resultado da API foram buscadas no último scan.

#### Como funciona

Devolve uma linha com a contagem de páginas (paginação via queryMore no
REST):

| Coluna | Tipo | Notas |
|---|---|---|
| `pages` | BIGINT | Páginas de resultado da API buscadas |

#### Para que serve

Muitas páginas indicam que o scan trouxe muitos registros; é um sinal de
que vale filtrar mais ou considerar o transporte Bulk.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account;
SELECT * FROM salesforce_last_scan_pages();
```

## Nível 4 - Funções utilitárias / autônomas

Estas funções recebem as credenciais como argumentos nomeados e **não**
exigem um `ATTACH`. São úteis para inspeção pontual de schema ou consultas
brutas.

### `salesforce_describe(object, client_id := ..., ...)`

#### O que faz

Descreve o schema de um único sObject sem anexar a org, retornando uma linha
por campo.

#### Como funciona

Recebe o nome do objeto e as credenciais como argumentos e devolve os
metadados de cada campo.

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

#### Para que serve

Permite inspecionar o schema de um único sObject sem anexar o catálogo
inteiro da org — bom para descobrir tipos e quais campos são filtráveis.

#### Uso no dia a dia

```sql
SELECT *
FROM salesforce_describe(
  'Account',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

### `salesforce_query(soql, client_id := ..., ...)`

#### O que faz

Executa uma consulta SOQL bruta e retorna cada registro de resultado como
uma string JSON, uma por linha.

#### Como funciona

É um utilitário de baixo nível: nenhum mapeamento de schema ou planejamento
de pushdown é aplicado. Os argumentos espelham os de `salesforce_describe`,
exceto que o primeiro argumento posicional é a string SOQL em vez do nome do
objeto; os mesmos argumentos nomeados de credencial se aplicam.

#### Para que serve

Útil quando você quer enviar um SOQL exatamente como escreveu, sem a camada
de tradução e tipagem da extensão.

#### Uso no dia a dia

```sql
SELECT *
FROM salesforce_query(
  'SELECT Id, Name FROM Account LIMIT 10',
  client_id     := 'KEY',
  client_secret := 'SECRET',
  refresh_token := 'TOKEN'
);
```

## Referência de pushdown

O planejador de scan aplica pushdown ao SOQL do máximo da consulta que for
seguro, e mantém o restante como um filtro **residual** avaliado no DuckDB.
Um filtro residual nunca altera a correção — significa apenas que mais
linhas são buscadas do que o estritamente necessário.

Com pushdown para o SOQL:

- **Projeção** — apenas os campos referenciados são requisitados.
- **Comparações** — `=`, `<>`, `<`, `<=`, `>`, `>=`.
- **Testes de nulo** — `IS NULL`, `IS NOT NULL`.
- **`AND`** de predicados elegíveis a pushdown.

Pushdown como pré-filtro superconjunto e depois mantido como residual (o
DuckDB reavalia exatamente):

- **`IN`** — pushdown como pré-filtro superconjunto, mantido residual.
- **`LIKE`** prefixo / sufixo / contém — pushdown como superconjunto,
  mantido residual.
- **`OR`** — pushdown apenas quando todos os filhos são, eles mesmos,
  seguros; enviado como superconjunto, mantido residual.

Mantido totalmente residual (sem pushdown):

- Chamadas de função em predicados.
- `NOT`.
- Predicados em campos não filtráveis.
- Uma cláusula WHERE que excederia 4000 caracteres ao ser renderizada como
  SOQL.

Agregações:

- `COUNT(*)` recebe pushdown como um `COUNT()` de SOQL (COUNT pushdown).
- Agregações diferentes de `COUNT(*)` **não** recebem pushdown.

Use `salesforce_query_cost()` (colunas `pushed_filters`, `residual_filters`,
`where_pushed`, `count_pushdown`) para ver exatamente o que chegou ao
Salesforce em um determinado scan.

## Funções de depuração / exclusivas de teste

> **DEBUG / SOMENTE-TESTE — não é uma API estável.** Tudo nesta seção existe
> para a própria suíte de testes da extensão e para depuração de baixo
> nível. Nomes, argumentos, formatos de saída e a própria existência podem
> mudar sem aviso. Não construa fluxos de produção sobre estas funções.

- `salesforce_describe_calls()` — DEBUG / SOMENTE-TESTE. Instrumentação de
  contagem de chamadas à API Describe.
- `salesforce_global_describe_calls()` — DEBUG / SOMENTE-TESTE.
  Instrumentação de contagem de chamadas à API Global Describe.
- `salesforce_tooling_calls()` — DEBUG / SOMENTE-TESTE. Instrumentação de
  contagem de chamadas à Tooling API.
- `salesforce_last_bulk_create_body()` — DEBUG / SOMENTE-TESTE. O corpo da
  requisição da chamada mais recente de criação de job Bulk. Não é uma API
  estável.
- `salesforce_decode(fields_json, records_json)` — DEBUG / SOMENTE-TESTE.
  Decodifica JSON bruto de campos/registros do Salesforce em linhas
  tipadas; usado para testar o caminho de decodificação isoladamente.
- `sf_url_encode(s)` — DEBUG / SOMENTE-TESTE. Codifica uma string em URL;
  usado para testar a construção de query string.

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
