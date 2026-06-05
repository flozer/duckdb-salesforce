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

As plataformas validadas no CI hoje são `linux_amd64` + `windows_amd64`
(baseline) e `osx_arm64` (extra). A extensão ainda não foi publicada no catálogo
community, então ainda não existe um `INSTALL ... FROM community`.

### macOS: bundle de certificados TLS

A extensão **sempre** verifica o certificado TLS do servidor — não existe
flag insegura para desligar essa checagem em nenhuma plataforma. No macOS,
porém, o OpenSSL (compilado via `vcpkg`) **não** traz um CA bundle padrão e
**não** lê o Keychain do macOS. Por causa disso, um `ATTACH` *live* pode
falhar na verificação de certificado.

A solução é apontar o OpenSSL para um CA bundle através da variável de
ambiente `SSL_CERT_FILE` (o OpenSSL também respeita `SSL_CERT_DIR`). Isso
apenas escolhe as âncoras de confiança — a verificação continua **ligada**,
não é um bypass:

```bash
export SSL_CERT_FILE=$(brew --prefix)/etc/openssl@3/cert.pem   # bundle do OpenSSL via Homebrew
export SSL_CERT_FILE=$(python3 -m certifi)                     # bundle certifi do Python
```

Um `ATTACH` live que falha a verificação no macOS já imprime exatamente essa
sugestão na mensagem de erro. **Linux** e **Windows** não precisam disso: o
Linux usa o bundle do sistema e o Windows usa o trust store do SO. Um trust
store zero-config baseado no Keychain do macOS é um follow-up planejado, ainda
não entregue.

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

### 3.1 Fontes de autenticação

Por padrão, o `ATTACH` lê as credenciais das próprias opções do comando
(como nos exemplos acima). A opção `auth_source` permite escolher de onde
elas vêm, o que é útil para não deixar segredos no texto SQL. Os quatro
modos são:

- **`'options'`** (padrão) — credenciais nas opções do `ATTACH`:
  `client_id`, `client_secret` e `refresh_token` (obrigatórios), mais os
  opcionais `login_url` e `api_version`.
- **`'env'`** — credenciais nas variáveis de ambiente `SF_CLIENT_ID`,
  `SF_CLIENT_SECRET` e `SF_REFRESH_TOKEN` (obrigatórias), mais a opcional
  `SF_LOGIN_URL`.
- **`'sfdx_url'`** — credenciais na variável `SF_SFDX_AUTH_URL`, no formato
  `force://<clientId>:<clientSecret>:<refreshToken>@<host-da-instancia>` (o
  `clientSecret` pode ser vazio).
- **`'jwt'`** — fluxo **OAuth 2.0 JWT bearer**, ideal para uso
  headless/CI/pipeline. **Sem refresh token**: a extensão assina um JWT
  curto (RS256) e o troca por um access token mantido em memória; diante de
  um `401`, re-assina um novo assertion. Exige `client_id` (o *issuer*),
  `username` (o *subject* — o usuário Salesforce a personificar) e uma chave
  privada, cujo caminho vem da variável de ambiente `SF_JWT_KEY_FILE`
  (recomendado) ou da opção inline `private_key_file` (apenas dev local). A
  chave deve ser **PEM RSA PKCS#1 ou PKCS#8 não criptografada**. `login_url`
  é opcional (padrão `https://login.salesforce.com`; use
  `https://test.salesforce.com` para sandbox).

`api_version` funciona em todos os modos. Com `'env'`, `'sfdx_url'` ou
`'jwt'`, você **não** precisa informar as opções de credencial no `ATTACH`
— a fonte escolhida vence.

> **Pré-requisito do modo `'jwt'`:** o Connected App deve estar
> **pré-autorizado** — *admin-approved*, ou o usuário pré-autorizado (a
> configuração de certificado digital / *"Use digital signatures"* do JWT).

#### Contexto 1 — Terminal

Exporte as variáveis no shell e então rode o DuckDB e o `ATTACH` com
`auth_source 'env'`:

```bash
export SF_CLIENT_ID=sua_consumer_key
export SF_CLIENT_SECRET=seu_consumer_secret
export SF_REFRESH_TOKEN=seu_refresh_token
duckdb
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

Como alternativa, use a URL SFDX da CLI do Salesforce e `auth_source
'sfdx_url'`:

```bash
export SF_SFDX_AUTH_URL=$(sf org display --verbose --json | jq -r .result.sfdxAuthUrl)
duckdb
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'sfdx_url');
```

Para o modo `'jwt'`, exporte o caminho da chave em `SF_JWT_KEY_FILE` e então
faça o `ATTACH` com `client_id` e `username`:

```bash
export SF_JWT_KEY_FILE=/secure/server.key
duckdb
```

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  auth_source 'jwt',
  client_id 'YOUR_CONSUMER_KEY',
  username  'svc@example.com'
);
```

Apenas para desenvolvimento local, você pode informar a chave inline com
`private_key_file` (em pipelines, prefira a env var):

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  auth_source 'jwt',
  client_id 'YOUR_CONSUMER_KEY',
  username  'svc@example.com',
  private_key_file '/secure/server.key'
);
```

#### Contexto 2 — Web UI / backend

O app ou container injeta as variáveis `SF_*` (por exemplo, a partir de um
secret manager). O backend abre o DuckDB e faz o `ATTACH` com `auth_source
'env'` — assim, **nenhuma credencial aparece no texto SQL**:

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

Para o modo `'jwt'`, o app ou container injeta `SF_JWT_KEY_FILE` (por
exemplo, um secret montado) e o backend faz o `ATTACH` sem caminho de chave
nem secret no SQL:

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  auth_source 'jwt',
  client_id 'YOUR_CONSUMER_KEY',
  username  'svc@example.com'
);
```

#### Contexto 3 — Python / pipeline

Defina as variáveis no ambiente do processo e faça o `ATTACH` com
`auth_source 'env'`:

```python
import os, duckdb

os.environ["SF_CLIENT_ID"] = "sua_consumer_key"
os.environ["SF_CLIENT_SECRET"] = "seu_consumer_secret"
os.environ["SF_REFRESH_TOKEN"] = "seu_refresh_token"

con = duckdb.connect()
con.execute(
    "ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'env')"
)
```

Para o modo `'jwt'`, defina `SF_JWT_KEY_FILE` no ambiente do processo e
informe `client_id` e `username` no `ATTACH`:

```python
import os, duckdb

os.environ["SF_JWT_KEY_FILE"] = "/secure/server.key"

con = duckdb.connect()
con.execute(
    "ATTACH 'salesforce://prod' AS sf (TYPE salesforce, auth_source 'jwt', "
    "client_id '...', username 'svc@example.com')"
)
```

#### Nota de segurança

Os valores das variáveis de ambiente e a URL SFDX nunca são logados, e
tokens ou secrets nunca aparecem em mensagens de erro. Os erros são claros:
uma variável de ambiente ausente é nomeada (apenas o nome, sem valor); uma
URL SFDX malformada é sinalizada sem ser ecoada; `invalid_grant` vira
"refresh token inválido, expirado ou revogado"; e `invalid_client` vira
"client_id / client_secret incorreto".

No modo `'jwt'`, a chave privada, o JWT montado e o assertion assinado nunca
são logados e nunca aparecem em mensagens de erro. Os erros continuam
claros: um caminho de chave ausente nomeia a opção/env var que faltou
(`SF_JWT_KEY_FILE` ou `private_key_file`); um arquivo de chave ausente
nomeia apenas o caminho; uma chave não-PEM, ilegível ou criptografada é
reportada sem ecoar o conteúdo; e `invalid_grant` vira "inválido, expirado,
ou o usuário não está autorizado neste Connected App".

#### Fora de escopo (neste corte)

Os seguintes recursos **não** são suportados nesta versão: fluxo OAuth via
browser/web, secret storage, chaves privadas criptografadas, gerenciador de
credenciais do sistema operacional e persistência de token/chave.

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

### Bulk e campos blob/base64

A Bulk API 2.0 query do Salesforce, no formato CSV, **não** retorna campos
blob/base64. Forçar um desses campos faz o próprio Salesforce devolver o
erro `Blob field not supported in Bulk V2 Query with CSV content type`. No
DuckDB, os campos `base64` do Salesforce mapeiam para o tipo `BLOB`.

Para evitar essa falha, a extensão tem um guard guiado por metadados: se um
campo cujo tipo é `base64` estiver projetado no scan — em qualquer
profundidade, inclusive dentro de um `STRUCT` de relacionamento pai — o
transporte Bulk é tratado como incompatível. O comportamento depende de
`sf_force_transport`:

- **`auto`** — ao detectar um campo `base64` projetado, o scan permanece em
  REST e **nunca** escolhe Bulk. O motivo registrado é
  `auto: bulk-incompatible (projected base64 field 'NAME') -> rest`.
- **`bulk`** (forçado) — a extensão falha com um erro claro **antes** de
  criar qualquer job: `projected base64 field 'NAME' is not supported by
  Bulk API 2.0 CSV; use 'rest' or 'auto'`.
- **`rest`** — inalterado. O REST retorna os campos blob como `base64` no
  JSON.

Se o campo `base64` **não** for projetado, o Bulk é permitido normalmente.

Orientação prática: para ler uma coluna blob/base64, use REST (o padrão) ou
simplesmente não a projete num scan Bulk. A decisão e o motivo aparecem em
`salesforce_last_transport()` e `salesforce_query_cost()` (colunas
`transport` e `reason`).

Nota de escopo: este guard é guiado por metadados e cobre apenas campos
`base64`. Outros objetos não suportados pelo Bulk não são pré-listados; se
forçados, vão aparecer como um erro claro do próprio job Bulk do Salesforce.

### REST e campos BODY blob/base64 (URL reference)

A subseção anterior (Bulk) é só metade do quadro. O REST também tem uma
limitação específica para campos **BODY** blob/base64 de objetos como
`Attachment.Body` e `ContentVersion.VersionData`: nessas colunas, uma query
REST do Salesforce não devolve o conteúdo em base64 inline — devolve uma
**URL reference** que aponta para o blob.

O scanner **não** segue essa URL: **não há download automático** de
anexo/blob. Selecionar um desses campos via REST levanta um erro claro e
documentado:

```text
Salesforce returned a URL reference for blob/base64 field 'NAME'; inline
BLOB decoding is not supported by REST query. Select non-blob fields or
fetch the blob URL outside the scanner.
```

Orientação prática: selecione apenas os campos **não-blob** do objeto no
scan, e busque o conteúdo do blob **fora do scanner**, diretamente pela API
do Salesforce, usando o `Id` do registro.

Importante: esta limitação é específica dos campos **BODY** blob retornados
como URL reference. Campos `base64` pequenos/inline que o REST realmente
devolve embutidos no JSON continuam decodificando para `BLOB` normalmente.

### Quadro combinado por transporte (campos BODY blob/base64)

Juntando os dois lados acima, para campos BODY blob/base64:

- **Bulk** (subseção anterior) — não suportado: o guard falha com erro claro
  ou, em `'auto'`, mantém o scan em REST.
- **REST** (esta subseção) — retorna uma URL reference, não decodificável
  inline; erro claro.

Resumo: campos blob body **não são lidos diretamente como bytes** por este
conector em **nenhum** transporte. Busque-os **fora do scanner**, pela API
do Salesforce, usando o `Id` do registro.

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

- A profundidade padrão é 1 (apenas o pai direto); use
  `sf_relationship_depth` para estender até o avô (veja abaixo).
- Lookups polimórficos são ignorados.
- Predicados sobre subcampos são avaliados como filtros residuais (no
  DuckDB).
- O padrão é `'off'`.

### Traversal de avô (profundidade 2)

Com `sf_relationships='parent'`, a configuração `sf_relationship_depth`
controla quantos níveis a expansão desce. O padrão é `1` (só o pai direto);
com `2`, o pai (single-target) de um relacionamento de pai single-target
também é expandido, como um STRUCT aninhado:

```sql
SET sf_relationships='parent';
SET sf_relationship_depth=2;

SELECT Account.Owner.Name
FROM sf.Contact
LIMIT 10;
```

Aqui o caminho é `Contact → Account → Owner` (um `User`). Ressalvas:

- A profundidade é limitada a 2 (não os 5 níveis do SOQL).
- Cada salto precisa ser single-target; relacionamentos polimórficos são
  pulados em qualquer nível.
- Predicados sobre subcampos (por exemplo, `Account.Owner.Name` no `WHERE`)
  continuam residuais, sem pushdown.
- O over-fetch cresce com a profundidade: projetar o STRUCT busca todos os
  campos escalares de cada nível expandido.

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

### Diagnosticar a expansão de relacionamentos

`salesforce_relationships()` explica o que a expansão de relacionamentos de
pai fez na **última resolução de schema** de um objeto (a resolução ocorre na
primeira referência ao objeto, não no scan). Use-a para entender o over-fetch,
quais relacionamentos foram pulados e quão fundo a expansão desceu:

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;
SELECT Id FROM sf.Contact LIMIT 1;   -- dispara a resolução de schema

SELECT row_type, relationship_name, parent_object, depth_level, status, reason, field_count
FROM salesforce_relationships();
```

Como ler a saída:

- A primeira linha é sempre `row_type = config`: traz `relationships_mode`
  (`off` / `parent`), o `relationship_depth` efetivo (`1`..`2`) e os contadores
  `expanded_count` / `skipped_count`. Essa linha é emitida **mesmo com**
  `sf_relationships = 'off'` — nesse caso você recebe **só** a linha `config`,
  então "off" nunca parece um resultado vazio ou quebrado.
- Cada linha `row_type = relationship` é um campo `reference` considerado.
  Quando `status = expanded`, a coluna `field_count` mostra quantos campos o
  STRUCT do pai tem e a `note` avisa do over-fetch: o pai é buscado **inteiro**
  (todos os campos escalares queryable), pois a projeção aninhada não recebe
  pushdown — selecionar `Account.Name` ainda busca o `Account` completo.
- Quando `status = skipped`, a coluna `reason` diz o porquê (`polymorphic`,
  `self_reference`, `cycle`, `name_collision`, `parent_not_describable`,
  `no_fields` ou `no_relationship_name`). É assim que você descobre por que uma
  coluna de relacionamento esperada não apareceu.
- A coluna `depth_level` distingue o pai direto (`1`) do avô (`2`), então você
  vê exatamente o que a profundidade 2 acrescentou.

Por exemplo, em `sf.Contact` com profundidade 2 você costuma ver `Account`
expandido em `depth_level = 1`, `Owner` expandido em `depth_level = 2`
(o `User` dono do `Account`) e `What` pulado com `reason = polymorphic`
(`parent_object` NULL, pois há mais de um alvo).

### Atualizar metadados em sessão longa (`salesforce_refresh_metadata`)

O conector resolve o schema de cada objeto **uma vez**, na primeira referência,
e o mantém em cache em memória pelo tempo do `ATTACH` (junto do *object listing*
global da org). Numa sessão curta isso é ótimo, mas numa sessão longa há *schema
drift*: alguém adiciona um campo *custom* em `Account`, ou cria um objeto novo,
e a sua sessão continua enxergando o schema antigo. Antes era preciso fazer
`DETACH` e `ATTACH` de novo.

`salesforce_refresh_metadata` resolve isso invalidando o cache em memória, para
que a **próxima** query rebusque os metadados — a função não faz a chamada de
rede sozinha, só limpa o cache.

```sql
-- adicionaram um campo em Account na org; re-descreve só Account na próxima query:
SELECT * FROM salesforce_refresh_metadata('sf', 'Account');

-- criaram objetos novos / mudou bastante coisa: descarta schemas + object listing:
SELECT * FROM salesforce_refresh_metadata('sf');
```

A distinção de escopo é o ponto principal:

- **Com `object`** o refresh é cirúrgico: só o schema daquele objeto é
  descartado; os demais objetos e o *object listing* continuam em cache.
- **Sem `object`** o refresh é global: o *object listing* e **todos** os
  schemas já resolvidos são descartados, então a próxima varredura de objetos e
  a próxima referência a qualquer objeto rebuscam tudo.

A função devolve uma linha com `catalog`, `scope` (`global` ou `object`) e
`object` (NULL no refresh global). O `catalog` precisa ser um catálogo
Salesforce anexado, senão você recebe um erro claro de alias inexistente ou de
catálogo que não é Salesforce.

### Agregados server-side explícitos (`salesforce_aggregate`)

Quando você só quer o agregado — um total, uma média, um mínimo/máximo ou uma
contagem — e não quer trazer as linhas para o DuckDB, a table function
`salesforce_aggregate(catalog, object, aggregates [, filter [, group_by]])`
pede o cálculo direto ao Salesforce e devolve **uma única linha** (ou, com
`group_by`, uma linha por grupo). Ela **reutiliza a sessão
autenticada** do catálogo que você já anexou (o primeiro argumento é o alias
do `ATTACH`, ex. `'sf'`), então não há credenciais na chamada.

Isto é **opt-in**, não um pushdown transparente: é você quem escolhe pedir o
agregado server-side. O `COUNT(*)` continua sendo o único agregado com
pushdown automático em um `SELECT` normal (seção 6); os demais agregados você
pede explicitamente com esta função.

Todos os argumentos são literais `VARCHAR`. Cada termo de `aggregates` pode ter
um alias no estilo SOQL (separado por espaço, ex. `MIN(AnnualRevenue) minRev`).
A saída traz **uma coluna por termo**, todas `VARCHAR`, nomeadas pelo alias do
termo — ou `expr0`, `expr1`, ... quando o termo não tem alias. Como os valores
voltam como texto, faça o cast no DuckDB:

```sql
SELECT
  CAST(minRev AS DECIMAL(18,2)) AS min_rev,
  CAST(maxRev AS DECIMAL(18,2)) AS max_rev,
  CAST(n AS BIGINT)             AS n
FROM salesforce_aggregate(
  'sf', 'Account',
  'MIN(AnnualRevenue) minRev, MAX(AnnualRevenue) maxRev, COUNT(Id) n');
```

O quarto argumento (opcional) é um corpo de `WHERE` SOQL **sem** a palavra
`WHERE` — útil para restringir o cálculo:

```sql
SELECT CAST(n AS BIGINT) AS n
FROM salesforce_aggregate(
  'sf', 'Account',
  'COUNT(Id) n',
  'Industry = ''Technology''');
```

O quinto argumento (opcional) é `group_by`: identifiers de campo **simples**
separados por vírgula (ex. `Industry` ou `Industry, Type`). Com ele, a função
monta `SELECT <group_by>, <aggregates> FROM <object> [WHERE <filter>] GROUP BY
<group_by>` e devolve **uma linha por grupo** — as colunas de **GROUP vêm
primeiro** (nomeadas pelo campo), depois as de aggregate (alias ou `expr0`,
`expr1`, ...), tudo ainda `VARCHAR`. Como `group_by` é **posicional depois de
`filter`**, para agrupar **sem** filtro passe uma string **vazia** no quarto
argumento:

```sql
SELECT Industry, CAST(n AS BIGINT) AS account_count
FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry')
ORDER BY account_count DESC;
```

Para combinar filtro e agrupamento, preencha os dois (filter não-vazio +
group_by):

```sql
SELECT Industry, CAST(n AS BIGINT) AS active_count
FROM salesforce_aggregate(
  'sf', 'Account',
  'COUNT(Id) n',
  'AnnualRevenue > 1000000',
  'Industry')
ORDER BY active_count DESC;
```

Funções permitidas: `MIN`, `MAX`, `SUM`, `AVG`, `COUNT` e `COUNT_DISTINCT`. A
função honra `sf_query_mode` e registra o SOQL nos diagnósticos
(`salesforce_last_soql()`, `salesforce_query_cost()`). Limites a conhecer:

- **Só termos de agregação.** Campos "nus" (sem função de agregação) são
  rejeitados — é o que mantém o contrato de uma única linha.
- **`GROUP BY` suportado** pelo argumento `group_by`, mas **só com campos
  simples**; dotted/relationship fields, expressões e `ROLLUP` / `CUBE` /
  `HAVING` continuam fora de escopo e são rejeitados com erro claro.
- O `object` deve ser um identifier válido; agregados de
  relacionamento/polymorphic vão direto ao SOQL e qualquer erro do Salesforce
  aparece como veio.

## 12. Lendo registros arquivados e excluídos (queryAll)

Por padrão, um scan vê apenas os registros vivos. Para também incluir os
registros **arquivados** e os **excluídos** por soft delete que o Salesforce
ainda mantém, mude o modo de leitura da sessão com `sf_query_mode`:

```sql
SET sf_query_mode='queryAll';

SELECT Id, Name, IsDeleted
FROM sf.Account
LIMIT 10;
```

- O padrão é `'query'` (só registros vivos); `'queryAll'` lê pela capacidade
  `queryAll` do Salesforce, que devolve também arquivados e deletados.
- Os deletados aparecem com `IsDeleted = true`, então você pode filtrar ou
  separar esses registros com SQL normal.
- O modo se aplica ao scan REST (endpoint `/queryAll`), ao scan Bulk (job com
  `operation: "queryAll"`) e às sondagens de `COUNT()` e de `MIN(Id)` /
  `MAX(Id)`. Por isso, o COUNT pushdown, o transporte `auto` e as faixas de
  PK do Bulk chunking passam a refletir também os deletados e arquivados.
- Um valor inválido gera um erro claro
  (`sf_query_mode must be 'query' or 'queryAll'`).

Caveat: isto **não** é histórico, CDC nem replicação, e não é um snapshot
local — apenas expõe a capacidade de leitura do Salesforce naquele scan. A
utility `salesforce_query()` é sempre `query` e ignora `sf_query_mode`.

## 13. Limitações

Conheça estes limites antes de construir sobre a extensão:

- **Somente-leitura.** Toda escrita e DDL contra o catálogo Salesforce
  lança erro.
- **`LIMIT` no Bulk não é server-side**; o job Bulk roda por completo (o
  streaming lazy ainda interrompe o download das páginas seguintes com um
  `LIMIT` pequeno).
- **auto não enxerga o `LIMIT`** ao sondar. Para uma consulta com `LIMIT`
  pequeno em um objeto enorme, force `rest`.
- **Relacionamentos são apenas de pai, profundidade até 2**
  (`sf_relationship_depth`, do pai ao avô); lookups polimórficos são
  ignorados e predicados sobre subcampos são residuais.
- **A origem de schema tooling produz tipos mais grosseiros** e reduz o
  pushdown.
- **O pushdown de agregação é apenas para COUNT.**
- **Os PK chunks não têm ordem global** e podem ser desiguais ou vazios.
- **As plataformas validadas no CI** são `linux_amd64` + `windows_amd64`
  (baseline) e `osx_arm64` (extra); `osx_amd64`, outros ARM e wasm ainda não
  foram validados. TLS ao vivo no macOS não é verificado (use `SSL_CERT_FILE`).
- **Ainda não está no catálogo community**, e o projeto é **pré-1.0**.
