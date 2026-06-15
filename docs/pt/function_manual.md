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
| `auth_source` | não | `'options'` | De onde o `ATTACH` obtém as credenciais OAuth: `'options'`, `'env'`, `'sfdx_url'` ou `'jwt'` |

Notas de comportamento:

- **Catálogo somente leitura.** Sem INSERT/UPDATE/DELETE/DDL nos sObjects
  anexados.
- **Lazy sObject resolution.** O schema de um sObject é buscado no primeiro
  uso, então anexar uma org grande é barato.
- Use `login_url` para apontar para um sandbox
  (`https://test.salesforce.com`) ou um host de login de My Domain.

##### Fonte das credenciais: `auth_source`

A opção `auth_source` decide **de onde** o `ATTACH` lê as credenciais OAuth.
Ela aceita quatro valores:

- **`'options'`** (padrão, inalterado) — as credenciais vêm das próprias
  opções do `ATTACH`: `client_id`, `client_secret` e `refresh_token`
  (obrigatórios), mais os opcionais `login_url` e `api_version`. É o
  comportamento descrito na tabela acima.
- **`'env'`** — as credenciais vêm de variáveis de ambiente. A extensão lê
  `SF_CLIENT_ID`, `SF_CLIENT_SECRET` e `SF_REFRESH_TOKEN` (obrigatórias),
  mais a opcional `SF_LOGIN_URL`. As opções de credencial do `ATTACH` não
  são necessárias neste modo.
- **`'sfdx_url'`** — as credenciais vêm da variável `SF_SFDX_AUTH_URL`, no
  formato `force://<clientId>:<clientSecret>:<refreshToken>@<host-da-instancia>`
  (o `clientSecret` pode ser vazio). As opções de credencial do `ATTACH`
  também não são necessárias neste modo.
- **`'jwt'`** — fluxo **OAuth 2.0 JWT bearer**, pensado para uso
  headless/CI/pipeline. A extensão assina via **RS256** um JWT curto
  (`iss` = `client_id`, `sub` = `username`, `aud` = `login_url`,
  `exp` = agora + 300s) e o troca em `<login_url>/services/oauth2/token`
  com `grant_type=urn:ietf:params:oauth:grant-type:jwt-bearer`. O retorno é
  um access token mantido **em memória**. **Não há refresh token**: diante de
  um `401`, a extensão simplesmente re-assina um novo assertion. As opções
  obrigatórias neste modo são `client_id` (o *issuer*), `username` (o
  *subject* — o usuário Salesforce a personificar) e uma chave privada.

  O caminho da chave privada vem de uma destas fontes:

  - a variável de ambiente **`SF_JWT_KEY_FILE`** (**recomendado** — mantém
    caminhos de chave fora do SQL; use em pipelines); ou
  - a opção inline **`private_key_file`** do `ATTACH` (aceita apenas para
    desenvolvimento local / ergonomia).

  A chave deve ser **PEM RSA PKCS#1 ou PKCS#8 não criptografada**. Chaves
  criptografadas **não** são suportadas (fora de escopo). `login_url` é
  opcional (padrão `https://login.salesforce.com`; use
  `https://test.salesforce.com` para sandbox) e `api_version` funciona como
  nos outros modos.

  **Pré-requisito:** o Connected App deve estar **pré-autorizado** — *admin-
  approved*, ou o usuário pré-autorizado (a configuração de certificado
  digital / *"Use digital signatures"* do JWT). Sem essa pré-autorização, a
  troca falha.

`api_version` funciona em **todos** os modos. Com `'env'`, `'sfdx_url'` ou
`'jwt'`, as opções de credencial do `ATTACH` não precisam ser informadas —
a fonte escolhida vence.

Exemplos com `'env'` e `'sfdx_url'`:

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'env');
```

```sql
ATTACH 'salesforce://production' AS sf (TYPE salesforce, auth_source 'sfdx_url');
```

Exemplo com `'jwt'` (caminho da chave via `SF_JWT_KEY_FILE`, recomendado):

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  auth_source 'jwt',
  client_id 'YOUR_CONSUMER_KEY',
  username  'svc@example.com'
);
```

Para desenvolvimento local, você pode informar a chave inline com
`private_key_file` (prefira a env var em pipelines):

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  auth_source 'jwt',
  client_id 'YOUR_CONSUMER_KEY',
  username  'svc@example.com',
  private_key_file '/path/to/server.key'
);
```

Notas de segurança e de erro:

- Os valores das variáveis de ambiente e a URL SFDX **nunca** são logados;
  tokens e secrets **nunca** aparecem em mensagens de erro.
- Variável de ambiente ausente: o erro nomeia **apenas** a variável que
  faltou, sem revelar nenhum valor.
- URL SFDX malformada: o erro avisa sobre o formato inválido **sem** ecoar a
  URL.
- `invalid_grant` na troca OAuth é traduzido como "refresh token inválido,
  expirado ou revogado".
- `invalid_client` é traduzido como "client_id / client_secret incorreto".

Notas de segurança e de erro específicas do modo `'jwt'`:

- A chave privada, o JWT montado e o assertion assinado **nunca** são
  logados e **nunca** aparecem em mensagens de erro.
- Caminho de chave ausente: o erro nomeia a opção/env var que faltou
  (`SF_JWT_KEY_FILE` ou `private_key_file`), sem revelar valor.
- Arquivo de chave ausente: o erro nomeia **apenas** o caminho.
- Chave não-PEM, ilegível ou criptografada: é reportada **sem** ecoar o
  conteúdo da chave.
- `invalid_grant` na troca JWT é traduzido como "inválido, expirado, ou o
  usuário não está autorizado neste Connected App".

Fora de escopo neste corte (não suportado): fluxo OAuth via browser/web,
secret storage, chaves privadas criptografadas, gerenciador de credenciais
do sistema operacional e persistência de token/chave.

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

#### Nota: Custom Metadata e Custom Settings

Custom Metadata Types (`__mdt`) e Custom Settings consultáveis (`__c`, tipos
List e Hierarchy) aparecem na listagem como **sObjects comuns, somente-leitura**
— sem sintaxe nem tratamento especial; `DESCRIBE` e `SELECT` funcionam como em
qualquer objeto. Isto é **acesso a dados, não a Metadata API**: o conector só
**lê** registros, não faz deploy nem altera metadados. A visibilidade segue as
**permissões** do usuário/org (metadados/settings protegidos podem não aparecer
ou não ser legíveis). Após adicionar um tipo `__mdt` ou um campo durante a
sessão, use `salesforce_refresh_metadata()` para que a próxima referência
re-descreva o schema.

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

#### Guard de compatibilidade Bulk para campos blob/base64

A Bulk API 2.0 query CSV do Salesforce não retorna campos blob/base64 — os
campos `base64` do Salesforce mapeiam para `BLOB` no DuckDB. Há um guard
guiado por metadados que detecta um campo `base64` projetado em qualquer
profundidade (inclusive dentro de um `STRUCT` de relacionamento pai) e
ajusta o transporte:

- Com `'auto'`, o scan permanece em REST e nunca escolhe Bulk; o motivo é
  `auto: bulk-incompatible (projected base64 field 'NAME') -> rest`.
- Com `'bulk'` (forçado), a extensão falha **antes** de criar o job com
  `projected base64 field 'NAME' is not supported by Bulk API 2.0 CSV; use
  'rest' or 'auto'` (caso contrário, o Salesforce devolveria `Blob field
  not supported in Bulk V2 Query with CSV content type`).
- Com `'rest'`, o comportamento é inalterado.

Se o campo `base64` não for projetado, o Bulk é permitido normalmente. A
decisão e o motivo ficam visíveis em `salesforce_last_transport()` e
`salesforce_query_cost()` (colunas `transport` e `reason`). O guard cobre
apenas campos `base64`; outros objetos não suportados pelo Bulk não são
pré-listados e, se forçados, aparecem como erro claro do job Bulk.

#### Limitação REST para campos BODY blob/base64 (URL reference)

O Bulk não é o único transporte com restrição em campos blob. Para campos
**BODY** blob/base64 de objetos como `Attachment.Body` e
`ContentVersion.VersionData`, uma query REST do Salesforce devolve uma **URL
reference** (não o base64 inline). O scanner **não baixa blobs**: não há
download automático ao seguir essa URL. Selecionar um desses campos via REST
levanta o erro:

```text
Salesforce returned a URL reference for blob/base64 field 'NAME'; inline
BLOB decoding is not supported by REST query. Select non-blob fields or
fetch the blob URL outside the scanner.
```

Quadro combinado por transporte para campos BODY blob/base64: o Bulk não é
suportado (guard acima) e o REST devolve uma URL reference não decodificável
inline. Em **nenhum** transporte este conector lê esses campos diretamente
como bytes — busque o conteúdo **fora do scanner**, pela API do Salesforce,
usando o `Id` do registro. A limitação é específica dos campos **BODY** blob
retornados como URL reference; campos `base64` pequenos/inline que o REST
devolve embutidos no JSON continuam decodificando para `BLOB` normalmente.

#### Decode de datetime/date/time (ISO 8601) nos dois transportes

Os campos `datetime`, `date` e `time` do Salesforce são esperados como
**strings ISO 8601** (ex. `2024-01-15T10:20:30.000+0000`, `2024-01-15`,
`10:20:30.000Z`) e decodificam para `TIMESTAMP` / `DATE` / `TIME` do DuckDB —
o **mesmo decode tipado** vale no REST (JSON) e no Bulk (CSV). Os timestamps
são normalizados para o valor **UTC wall-clock** (o offset de timezone é
removido, não deslocado). Um valor **numérico / epoch** (ex. `1705314030000`)
num campo de tempo **não** é interpretado: o conector não adivinha
segundos-vs-milissegundos e levanta um erro claro, nomeando o campo e o tipo
(`field 'Whn' (Salesforce type 'datetime') could not be decoded as
TIMESTAMP`), sem ecoar o valor ofensor. Para tratar um valor de tempo
numérico, transforme-o **fora do scanner** (por exemplo, `to_timestamp` do
DuckDB sobre o número bruto).

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

### `sf_bulk_poll_budget`

#### O que faz

Limita quantas vezes um job Bulk é consultado (poll) até concluir antes de
falhar rápido.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `600` (~250 ms por poll)

`BulkStartJob` consulta o status do job até `JobComplete`. Quando a contagem
atinge o orçamento, levanta um erro claro que nomeia esta configuração, em
vez de consultar para sempre. A contagem ao vivo aparece em
`salesforce_query_cost().bulk_polls`, então um job que estoura o tempo mostra
até onde chegou.

#### Para que serve

Uma janela grande de backfill pode legitimamente precisar de mais de 600
polls; aumente o orçamento para ela. Reduza para falhar mais rápido em testes
ou sessões interativas.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_poll_budget = 2000;   -- deixa um job demorado concluir

SELECT Id FROM sf.BigObject__c
WHERE CreatedDate >= TIMESTAMP '2024-01-01 00:00:00'
  AND CreatedDate <  TIMESTAMP '2024-02-01 00:00:00';

SELECT bulk_polls FROM salesforce_query_cost();
```

### `sf_bulk_require_predicate`

#### O que faz

Recusa uma leitura Bulk que **não** empurra nenhum predicado para o SOQL
(extração do objeto inteiro), antes de criar qualquer job.

#### Como funciona

- Tipo: `BOOLEAN`
- Padrão: `false` (apenas orientação)

Quando `true`, um scan Bulk com `where_pushed` vazio falha rápido com um
`BinderException`. O padrão `false` mantém o comportamento inalterado — a
`salesforce_query_cost().guidance` ainda alerta sobre a leitura sem filtro. O
guard checa o predicado **empurrado**, não o texto SQL: um `WHERE` que fica
residual (ex.: uma função sobre a coluna, veja a seção 13) não o satisfaz.

#### Para que serve

Para backfills grandes planejados: força toda leitura Bulk a passar por uma
janela empurrada de `CreatedDate` / `SystemModstamp`, de modo que um scan
acidental do objeto inteiro apareça como erro, não como over-fetch de milhões
de linhas.

#### Uso no dia a dia

```sql
SET sf_force_transport = 'bulk';
SET sf_bulk_require_predicate = true;

-- falha rápido: nenhum predicado empurrado
-- SELECT Id FROM sf.BigObject__c;

-- prossegue: a faixa de CreatedDate é empurrada para o SOQL
SELECT Id FROM sf.BigObject__c
WHERE CreatedDate >= TIMESTAMP '2024-01-01 00:00:00'
  AND CreatedDate <  TIMESTAMP '2024-02-01 00:00:00';
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

Por padrão, a expansão tem um nível (apenas o pai direto). A profundidade
da travessia é controlada pela configuração `sf_relationship_depth`, que
permite estender a expansão até o avô (grandparent).

#### Para que serve

Facilita ler campos do registro-pai (por exemplo, o `Account` de um
`Contact`) sem fazer joins manuais.

#### Uso no dia a dia

```sql
SET sf_relationships = 'parent';
SELECT Id, Email FROM sf.Contact;
```

### `sf_relationship_depth`

#### O que faz

Define até quantos níveis de relacionamento-pai a expansão de STRUCT vai
descer. Só tem efeito quando `sf_relationships = 'parent'`.

#### Como funciona

- Tipo: `BIGINT`
- Padrão: `1`
- Valor máximo: `2`

Com `1` (padrão), apenas o pai direto é expandido — o comportamento é
idêntico ao de `sf_relationships = 'parent'` (um nível de STRUCT de pai).

Com `2`, a expansão também desce ao **avô (grandparent)**: o pai
(single-target) de um relacionamento de pai single-target vira um STRUCT
**aninhado**. Por exemplo, `Account.Owner.Name` em `sf.Contact` percorre
`Contact → Account → Owner` (um `User`).

Regras da travessia:

- Cada salto precisa ser single-target; **relacionamentos polimórficos são
  pulados em qualquer nível**, assim como relacionamentos para si mesmo
  (self) e ciclos.
- A profundidade é limitada estritamente a `2` (não os 5 níveis que o
  Salesforce permite em SOQL).
- Predicados sobre subcampos continuam **residuais** (não há pushdown de,
  por exemplo, `Account.Owner.Name` no `WHERE`), igual ao pai de um nível.
- O over-fetch cresce com a profundidade: ao projetar o STRUCT, a extensão
  busca todos os campos escalares de cada nível expandido (mesmo trade-off
  documentado para o pai de um nível).
- A expansão acontece somente no caminho de **describe**; o schema obtido
  via Tooling continua flat, sem expansão. Os describes de pai e de avô
  reutilizam o cache por `ATTACH`.
- O transporte REST decodifica o JSON aninhado; o Bulk decodifica os
  headers CSV aninhados (`Account.Owner.Name`).

#### Para que serve

Permite ler campos do avô (por exemplo, o dono `User` do `Account` de um
`Contact`) em uma única consulta, sem joins manuais e sem precisar expandir
o objeto intermediário separadamente.

#### Uso no dia a dia

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;
SELECT Account.Owner.Name FROM sf.Contact LIMIT 10;
```

### `sf_query_mode`

#### O que faz

Escolhe se um scan lê apenas os registros vivos do sObject ou se também
inclui os registros arquivados e excluídos (soft delete) que o Salesforce
ainda mantém.

#### Como funciona

- Tipo: `VARCHAR`
- Padrão: `'query'`
- Valores aceitos: `'query'` ou `'queryAll'`

Com `'query'` (padrão), o comportamento é inalterado: o scan vê só os
registros vivos. Com `'queryAll'`, o scan passa a ler pela capacidade
**queryAll** do Salesforce, que também devolve os registros **arquivados**
e os **excluídos** por soft delete (`IsDeleted = true`), além dos vivos.

O modo se aplica a todo o caminho do scan: ao scan REST (endpoint
`/queryAll`), ao scan Bulk (job com `operation: "queryAll"`) e também às
sondagens de `COUNT()` e de `MIN(Id)` / `MAX(Id)`. Por isso, o COUNT
pushdown, a seleção de transporte `'auto'` e as faixas de PK do Bulk
chunking passam a refletir os registros deletados e arquivados quando
`'queryAll'` está ativo.

Não é histórico, CDC nem replicação, e não é um snapshot local: apenas
expõe a capacidade de leitura do Salesforce naquele scan. Um valor inválido
gera um erro claro (`sf_query_mode must be 'query' or 'queryAll'`).

#### Para que serve

Útil quando você precisa enxergar registros que o Salesforce removeu por
soft delete ou arquivou — por exemplo, para auditoria ou conciliação — sem
mudar nada no comportamento dos demais scans da sessão.

#### Uso no dia a dia

```sql
SET sf_query_mode = 'queryAll';
SELECT Id, Name, IsDeleted FROM sf.Account LIMIT 10;
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
| `query_mode` | VARCHAR | Modo de leitura usado (`query` / `queryAll`) |
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
| `guidance` | VARCHAR | Conselho legível por humanos; diz se o Salesforce filtrou server-side ou se o DuckDB filtrou após um scan remoto completo |
| `bulk_polls` | BIGINT | Contagem de polls de status do job Bulk (NULL em scans não-Bulk) |

#### Para que serve

É a primeira parada quando uma consulta está lenta ou cara: mostra o que
realmente chegou ao Salesforce e sugere como ajustar o scan.

#### Uso no dia a dia

```sql
SELECT Id, Name FROM sf.Account WHERE Industry = 'Technology';
SELECT * FROM salesforce_query_cost();
```

### `salesforce_query_explain()`

#### O que faz

Explica o **último scan** campo a campo: uma linha por campo projetado e por
filtro conjuntivo, mais linhas meta para travessia de relacionamento, count
pushdown e transporte. Onde `salesforce_query_cost()` é uma única linha de
resumo, `salesforce_query_explain()` é o detalhamento por campo — diz, campo a
campo, **se o Salesforce filtrou no servidor (`pushed`) ou se o DuckDB filtrou
localmente após um scan remoto completo (`residual`)**. Uma linha de filtro
`residual` é o sinal de alerta de over-fetch.

É **somente leitura e apenas diagnóstico**: reflete um snapshot que o scan grava
e não altera nada da execução. **Não** é um recurso de performance — explica, não
acelera.

#### Como funciona

Sem argumentos. Retorna uma linha por item do último scan. Colunas:

| Coluna | Tipo | Notas |
|---|---|---|
| `object_name` | VARCHAR | sObject lido |
| `field_name` | VARCHAR | Campo; NULL para predicado complexo ou linha meta |
| `role` | VARCHAR | `projection` \| `filter` \| `relationship` \| `count` \| `transport` |
| `resolved` | BOOLEAN | Campo resolvido nos metadados (NULL nas linhas `count`/`transport`) |
| `filterable` | BOOLEAN | Flag do Describe (NULL se não resolvido / linha meta) |
| `sortable` | BOOLEAN | Flag do Describe (NULL se não resolvido / linha meta) |
| `relationship_name` | VARCHAR | Nome do relacionamento single-hop, quando aplicável |
| `reference_to` | LIST(VARCHAR) | Objeto(s)-alvo do relacionamento; vazio caso contrário |
| `pushed` | BOOLEAN | Empurrado para o SOQL (SELECT para projeção, WHERE para filtro) |
| `residual` | BOOLEAN | Reaplicado pelo DuckDB após o scan |
| `reason` | VARCHAR | Token de motivo fechado (ver abaixo) |
| `guidance` | VARCHAR | Dica curta e acionável |

**Roles:** `projection` (campo selecionado), `filter` (conjunto do WHERE),
`relationship` (relacionamento pai realmente atravessado), `count` (uma linha com
o estado de count-pushdown), `transport` (uma linha com REST/Bulk).

**Reasons (conjunto fechado):**

| Reason | Significado |
|---|---|
| `pushed_to_soql` | Filtro traduzido para o WHERE do SOQL (servidor) |
| `projected` | Coluna incluída no SELECT do SOQL |
| `not_filterable` | Campo não é `filterable`; o DuckDB filtra residualmente (over-fetch) |
| `unsupported_operator` | Operador/forma que o SOQL não expressa; residual |
| `complex_expression` | Predicado OR/NOT/cross-field/aninhado mantido residual (campo NULL) |
| `unresolved_field` | Campo não encontrado nos metadados do objeto |
| `metadata_unavailable` | Catálogo/engine indisponível; anotação degradada (sem erro) |
| `relationship_traversed` | Um relacionamento pai single-hop foi expandido |
| `count_pushdown` | Contagem servida por `SELECT COUNT()` (sem buscar registros) |
| `count_not_pushed` | Não é um scan apenas de contagem |
| `transport_rest` | Transporte REST foi usado |
| `transport_bulk` | Transporte Bulk API 2.0 foi usado |

#### Último scan, sem parâmetros, comportamento sem scan

- Reporta o **scan mais recente desta sessão** (escopo de processo, como
  `salesforce_query_cost()`). **Não** recebe parâmetros — explicar uma query
  arbitrária está fora de escopo (exigiria planejar a query sem executá-la).
- **Antes de qualquer scan retorna zero linhas** — nunca fabrica uma linha
  `transport`/`count` padrão.
- A anotação de metadados usa o Metadata Engine compartilhado (ver
  `salesforce_metadata_fields()`); se o catálogo for desanexado ou os metadados
  ficarem indisponíveis, as linhas anotadas pela engine degradam para
  `reason = metadata_unavailable` (as linhas `count`/`transport` continuam
  aparecendo). Nunca lança erro.

#### Não diagnosticável: `LIMIT`

`LIMIT` não aparece em `salesforce_query_explain()`. Neste build do DuckDB a table
function nunca recebe o `LIMIT` da query — o DuckDB o aplica acima do scan — então
não há estado de scan para reportá-lo. É omitido de propósito, não chutado.

#### Por que usar

`salesforce_query_cost()` diz *quanto* foi empurrado; `query_explain()` diz
*quais campos* e *por quê*. Use para confirmar que um predicado chegou ao
Salesforce (`pushed_to_soql`) em vez de arrastar o objeto inteiro pela rede
(linhas residuais `not_filterable` / `complex_expression`).

#### Uso no dia a dia

```sql
SELECT count(*) FROM sf.Account WHERE Industry = 'Technology';
SELECT object_name, field_name, role, pushed, residual, reason
FROM salesforce_query_explain()
ORDER BY role, field_name;
```

### `salesforce_relationships()`

#### O que faz

Reporta o que a expansão de relacionamentos de pai (parent relationship) fez
na **última vez** que o schema de um sObject foi resolvido: quais
relacionamentos `reference` foram expandidos em STRUCT, quais foram pulados e
por quê. É um diagnóstico **puro** — não muda absolutamente nada no
comportamento da extensão.

#### Como funciona

A resolução de schema acontece na **primeira referência** ao objeto (um
`SELECT` ou um `DESCRIBE`). Esta função reflete o objeto resolvido mais
recentemente. Reconsultar um schema que já está em cache **não** re-resolve,
então o instantâneo não muda nesse caso — para ver outro objeto, force a
resolução dele referenciando-o pela primeira vez.

A saída segue um modelo de duas naturezas de linha, com um schema de colunas
**uniforme** (as colunas que não se aplicam à linha ficam `NULL`):

- exatamente **uma** linha `config`, sempre emitida — mesmo com
  `sf_relationships = 'off'`. Assim, "off" nunca parece um resultado vazio ou
  quebrado: você recebe a linha `config` informando o modo, e nenhuma linha
  `relationship`.
- depois, **uma** linha `relationship` por campo `reference` considerado na
  resolução (cada uma com `status` `expanded` ou `skipped`).

Colunas de saída:

| Coluna | Tipo | Linha `config` | Linha `relationship` |
|---|---|---|---|
| `row_type` | VARCHAR | `'config'` | `'relationship'` |
| `object` | VARCHAR | objeto resolvido | objeto resolvido |
| `relationships_mode` | VARCHAR | valor de `sf_relationships` (`off` / `parent`) | NULL |
| `relationship_depth` | BIGINT | `sf_relationship_depth` efetivo (`1`..`2`) | NULL |
| `relationship_name` | VARCHAR | NULL | `relationshipName` do SF (ou o nome do campo) |
| `parent_object` | VARCHAR | NULL | objeto alvo; NULL se polymorphic |
| `depth_level` | BIGINT | NULL | `1` = parent, `2` = grandparent |
| `status` | VARCHAR | NULL | `'expanded'` ou `'skipped'` |
| `reason` | VARCHAR | NULL | NULL se expanded; senão o motivo do skip |
| `field_count` | BIGINT | NULL | nº de campos do STRUCT quando expanded; NULL se skipped |
| `expanded_count` | BIGINT | nº de relacionamentos expandidos | NULL |
| `skipped_count` | BIGINT | nº de relacionamentos pulados | NULL |
| `note` | VARCHAR | resumo | nota de over-fetch (linhas expanded) |

Motivos de skip (coluna `reason` nas linhas `skipped`):

- `polymorphic` — o relacionamento tem mais de um alvo, então não há um STRUCT
  único para expandir.
- `self_reference` — o relacionamento aponta para o próprio objeto.
- `cycle` — o pai já está no caminho de expansão atual.
- `name_collision` — o `relationshipName` colide com uma coluna já existente.
- `parent_not_describable` — o describe do objeto-pai falhou.
- `no_fields` — o pai não tem campos úteis para expandir.
- `no_relationship_name` — o relacionamento não expõe um `relationshipName`.

#### A nota de over-fetch

Este é o insight principal da função. Um pai expandido é buscado como um STRUCT
**completo**, contendo **todos** os campos escalares queryable do pai —
inclusive as colunas de id de chave estrangeira. A coluna `field_count` reflete
exatamente esse número. A projeção aninhada **não** é empurrada para o SOQL,
então selecionar um único subcampo (por exemplo `Account.Name`) ainda busca o
`Account` inteiro. A coluna `note` nas linhas `expanded` avisa sobre isso — é
assim que você identifica e quantifica o over-fetch.

#### Para que serve

Complementa `salesforce_query_cost()`, mas é uma função **separada de
propósito**: aquela mede o custo de um **scan** (outro ciclo de vida), enquanto
esta explica a **resolução de schema** — o que a expansão de relacionamentos
realmente produziu. Use-a para entender por que uma coluna de relacionamento
não apareceu (qual `reason` de skip), quão fundo a expansão desceu
(`depth_level`) e quanto over-fetch cada STRUCT acrescenta (`field_count`).

#### Uso no dia a dia

```sql
SET sf_relationships = 'parent';
SET sf_relationship_depth = 2;
SELECT Id FROM sf.Contact LIMIT 1;   -- dispara a resolução de schema

SELECT row_type, relationship_name, parent_object, depth_level, status, reason, field_count
FROM salesforce_relationships();
```

Lendo um resultado típico de `Contact`:

- uma linha `config` com `relationships_mode = parent`, `relationship_depth = 2`
  e os contadores `expanded_count` / `skipped_count`;
- uma linha `relationship` para `Account`, `status = expanded`, `depth_level = 1`
  (pai direto), com `field_count` e a `note` de over-fetch;
- uma linha `relationship` para `What`, `status = skipped`,
  `reason = polymorphic`, `parent_object` NULL (mais de um alvo);
- uma linha `relationship` para `Owner`, `status = expanded`, `depth_level = 2`
  (grandparent — o `User` dono do `Account`).

Com `sf_relationships = 'off'`, a mesma consulta devolve **somente** a linha
`config` (com `relationships_mode = off`), e nenhuma linha `relationship`.

### `salesforce_relationship_graph(catalog, object [, max_depth] [, include_children := false] [, direction := 'parent'])`

#### O que faz

Enumerador **sob demanda**, somente leitura, das relações de um objeto, obtido
pelo Metadata Engine compartilhado (REST Describe). Uma linha por aresta, cada
uma com um `status` explícito. Diferente de `salesforce_relationships()` (que
reflete a expansão de pais do *último scan*), esta percorre o grafo de **qualquer**
objeto que você nomear, independente de `sf_relationships`. Metadata pura; **sem
mudança de comportamento**.

#### Como funciona

As relações de pai são percorridas em profundidade até `max_depth` (padrão `1`,
limitado a `[1,4]`). Com `include_children := true`, as relações **filhas
diretas** do objeto também são listadas (um nível, sem recursão — relações
filhas se multiplicam muito).

`direction := 'parent' | 'child' | 'both'` filtra quais lados retornam
(case-insensitive). **Vence** sobre `include_children` quando ambos são
informados; senão `include_children := true` significa `both` e o padrão é
`parent`. Use `direction := 'child'` para listar só relações filhas sem o ruído
dos pais. `max_depth` se aplica à travessia de pais; linhas filhas são sempre de
nível raiz. Colunas de saída:

| Coluna | Tipo | Notas |
|---|---|---|
| `source_object` | VARCHAR | objeto de onde a aresta parte |
| `relationship_name` | VARCHAR | `relationshipName`; NULL para um filho sem nome |
| `path` | VARCHAR | caminho pontilhado a partir da raiz (ex.: `Account.Owner`) |
| `depth_level` | INTEGER | 1 = direto, 2 = avô, … |
| `target_object` | VARCHAR | objeto relacionado resolvido; NULL se polimórfico / não resolvido |
| `reference_to` | LIST(VARCHAR) | alvo(s) do pai; para um filho, o campo FK de volta |
| `direction` | VARCHAR | `parent` ou `child` |
| `relationship_type` | VARCHAR | `reference` (pai) ou `childRelationship` |
| `status` | VARCHAR | veja abaixo |
| `caveat` | VARCHAR | motivo curto; NULL quando `resolved` |

`status` (conjunto fechado): `resolved`, `polymorphic` (múltiplos `referenceTo`,
reportado mas não percorrido), `self_reference` (auto-pai direto), `cyclic`
(alvo já no caminho), `not_queryable` (alvo ausente no Describe Global),
`not_describable` (Describe do alvo falhou), `unnamed_child` (relação filha sem
`relationshipName` — não endereçável por subconsulta SOQL; diagnóstico, não erro).

#### Por que usar

Entender como um objeto se conecta antes de escrever SOQL/joins: quais pais são
single-hop seguros, quais são polimórficos ou cíclicos, e quais filhos existem.
`include_children` é **opt-in** — omitido, a saída é só de pais.

#### Uso diário

```sql
-- só pais (padrão), dois níveis
SELECT path, target_object, status
FROM salesforce_relationship_graph('sf', 'Contact', 2)
ORDER BY path;

-- inclui as relações filhas diretas do objeto
SELECT path, target_object, direction, status
FROM salesforce_relationship_graph('sf', 'Account', include_children := true)
WHERE direction = 'child';
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

### `salesforce_metadata_objects(catalog)`

#### O que faz

Lista todos os sObjects globais da org anexada com sua flag `queryable`. Um
diagnóstico de metadados somente leitura para analistas — "o que dá para
consultar aqui?".

#### Como funciona

Retorna uma linha por sObject. Servido pelo **Metadata Engine** compartilhado (um
Describe Global de-duplicado por catálogo), então chamadas repetidas não
rebuscam até `salesforce_refresh_metadata()` invalidar o cache.

| Coluna | Tipo | Notas |
|---|---|---|
| `object_name` | VARCHAR | Nome de API do sObject |
| `queryable` | BOOLEAN | Flag real do Describe Global (aparecem `true` e `false`) |

#### Uso no dia a dia

```sql
SELECT object_name FROM salesforce_metadata_objects('sf') WHERE queryable ORDER BY object_name;
```

### `salesforce_metadata_fields(catalog, object)`

#### O que faz

Lista os campos de um sObject com os metadados que guiam pushdown e decisões de
relacionamento — tipo, `filterable`, `sortable`, nome e alvo do relacionamento, e
valores de picklist. Somente leitura.

#### Como funciona

Retorna uma linha por campo, servido pelo Metadata Engine compartilhado (um
Describe por objeto, cacheado por catálogo).

| Coluna | Tipo | Notas |
|---|---|---|
| `object_name` | VARCHAR | O sObject |
| `field_name` | VARCHAR | Nome de API do campo |
| `type` | VARCHAR | Tipo do campo no Salesforce |
| `filterable` | BOOLEAN | Pode ser usado num `WHERE` empurrado para o SOQL |
| `sortable` | BOOLEAN | Pode ser usado num `ORDER BY` empurrado |
| `relationship_name` | VARCHAR | Nome do relacionamento pai (NULL se não for referência) |
| `reference_to` | LIST(VARCHAR) | Objeto(s)-alvo; alvos polimórficos são listados, não resolvidos; vazio quando não é referência |
| `picklist_values` | LIST(VARCHAR) | Valores de picklist permitidos; vazio para campos não-picklist |

Um campo sem alvos de relacionamento / sem valores de picklist retorna uma
**lista vazia**, nunca NULL.

#### Por que usar

`filterable` diz se um `WHERE` naquele campo será empurrado para o Salesforce ou
aplicado residualmente no DuckDB — exatamente a distinção que
`salesforce_query_explain()` reporta por query.

#### Uso no dia a dia

```sql
SELECT field_name, type, filterable, reference_to
FROM salesforce_metadata_fields('sf', 'Account')
ORDER BY field_name;
```

### `salesforce_refresh_metadata(catalog [, object])`

#### O que faz

Invalida o cache de metadados em memória de uma org anexada, de modo que a
**próxima** referência rebusque os metadados do Salesforce. A função em si
**não** faz chamada de rede.

#### Como funciona

O conector mantém, por `ATTACH`, um cache de metadados **em memória**: os
schemas dos objetos (resolvidos de forma lazy na primeira referência), o
*object listing* global da org e os *describes* dos objetos-pai usados na
expansão de relacionamentos. Não há cache de **dados** nem cache em **disco**
— só esses metadados.

Esta função apenas **limpa** esse cache; ela não busca nada por conta
própria. Quem dispara o rebusca é a próxima query que tocar o metadado
invalidado.

Argumentos:

| Argumento | Obrigatório | Significado |
|---|---|---|
| `catalog` | sim | Alias do `ATTACH` de uma org Salesforce anexada (posicional) |
| `object` | não | Nome de API de um sObject; quando omitido, o refresh é global (posicional) |

O escopo depende de `object` ter sido informado ou não:

- **`object` omitido → refresh global.** Descarta o *object listing* **e**
  todos os schemas já resolvidos (e os *describes* de pai). A próxima varredura
  de listing rebusca a lista de objetos; a próxima referência a **qualquer**
  objeto re-descreve o schema dele.
- **`object` informado → refresh só desse objeto.** Limpa apenas o schema
  resolvido daquele objeto (e a sua entrada de *parent-describe*). Os demais
  objetos e o *object listing* global ficam **intactos**.

Colunas de saída — devolve **uma** linha:

| Coluna | Tipo | Notas |
|---|---|---|
| `catalog` | VARCHAR | O alias informado |
| `scope` | VARCHAR | `'global'` ou `'object'` |
| `object` | VARCHAR | Nome do objeto, ou NULL no refresh global |

Erros (claros, sem expor segredos): o `catalog` precisa ser um catálogo
Salesforce anexado. Caso contrário você recebe `no attached catalog named
'<x>'` (não existe catálogo com esse alias) ou `catalog '<x>' is not a
Salesforce catalog` (o alias existe, mas não é uma org Salesforce).

#### Para que serve

Permite capturar mudanças de schema na org — um campo *custom* novo, um objeto
novo — **dentro de uma sessão longa**, sem precisar de `DETACH` seguido de
`ATTACH`. Como os schemas são resolvidos e cacheados na primeira referência,
sem esse refresh a sessão continuaria enxergando o schema antigo.

#### Uso no dia a dia

```sql
-- após adicionar um campo em Account no Salesforce:
SELECT * FROM salesforce_refresh_metadata('sf', 'Account');  -- re-descreve Account na próxima query
-- ou atualizar tudo (schemas + object listing):
SELECT * FROM salesforce_refresh_metadata('sf');
```

### `salesforce_picklist_values(catalog, object, field)`

#### O que faz

Retorna o catálogo de valores de um campo **picklist** de um sObject, uma
linha por valor — incluindo valores **ativos e inativos**.

#### Como funciona

A função reutiliza a sessão autenticada de um catálogo já anexado por
`ATTACH` (o primeiro argumento é o alias desse catálogo) e lê os valores a
partir do **REST describe** do objeto — o mesmo describe que carrega
`picklistValues` por campo. Não é a Metadata API: **sem SOAP, sem Tooling**.
É **somente leitura**; não faz deploy, retrieve nem CRUD.

Argumentos (todos posicionais):

| Argumento | Obrigatório | Significado |
|---|---|---|
| `catalog` | sim | Alias do `ATTACH` de uma org Salesforce anexada; a auth desse catálogo é reutilizada |
| `object` | sim | Nome de API do sObject (ex. `Account`) |
| `field` | sim | Nome de API do campo picklist (ex. `Industry`) |

Colunas de saída — uma linha por valor de picklist:

| Coluna | Tipo | Notas |
|---|---|---|
| `value` | VARCHAR | O valor de API da entrada de picklist |
| `label` | VARCHAR | O rótulo exibido da entrada |
| `active` | BOOLEAN | Se o valor está ativo |
| `is_default` | BOOLEAN | Se o valor é o default do campo |

#### Cache por `ATTACH` e refresh

O describe raw do objeto é buscado **uma vez por objeto** e reutilizado por
esta função, por `salesforce_record_types()` e por chamadas repetidas — é um
cache **em memória**, por `ATTACH`. Para forçar um rebusca depois de uma
mudança de metadados na org, use `salesforce_refresh_metadata(catalog[,
object])`, que limpa o cache; a próxima chamada re-descreve o objeto.

#### Escopo do picklist

O escopo é o catálogo **completo** do campo: a função devolve os valores
**ativos e inativos**, e `is_default` marca o default. Ela **não** filtra por
record type e **não** resolve dependent picklists (limitação documentada).
Para ver só os valores ativos, filtre com `WHERE active`.

#### Erros e casos de borda

- Alias de catálogo desconhecido, ou um alias que não é um catálogo
  Salesforce, ou um campo que não existe no objeto → **erro claro** (sem
  expor segredos).
- Um campo que **existe mas não é picklist** → **0 linhas** (não é erro).

#### Para que serve

Permite inspecionar, direto no SQL, quais valores um campo picklist aceita —
útil para validar dados, montar filtros ou documentar o domínio de um campo —
sem abrir o Setup do Salesforce.

#### Uso no dia a dia

```sql
-- todos os valores de picklist de Industry (ativos + inativos)
SELECT value, label FROM salesforce_picklist_values('sf', 'Account', 'Industry');
-- só ativos
SELECT value FROM salesforce_picklist_values('sf', 'Account', 'Industry') WHERE active;
```

### `salesforce_record_types(catalog, object)`

#### O que faz

Retorna os **record types** de um sObject, uma linha por record type.

#### Como funciona

Como `salesforce_picklist_values()`, reutiliza a sessão autenticada de um
catálogo anexado por `ATTACH` (primeiro argumento) e lê a partir do **REST
describe** do objeto — o mesmo describe que carrega `recordTypeInfos` por
objeto. Não é a Metadata API: **sem SOAP, sem Tooling**. É **somente
leitura**.

Argumentos (todos posicionais):

| Argumento | Obrigatório | Significado |
|---|---|---|
| `catalog` | sim | Alias do `ATTACH` de uma org Salesforce anexada; a auth desse catálogo é reutilizada |
| `object` | sim | Nome de API do sObject (ex. `Account`) |

Colunas de saída — uma linha por record type:

| Coluna | Tipo | Notas |
|---|---|---|
| `developer_name` | VARCHAR | O Developer Name do record type |
| `label` | VARCHAR | O rótulo exibido do record type |
| `record_type_id` | VARCHAR | O Id do record type |
| `active` | BOOLEAN | Se o record type está ativo |
| `is_default` | BOOLEAN | Se é o record type default |

#### Cache por `ATTACH` e refresh

O describe raw do objeto é buscado **uma vez por objeto** e compartilhado com
`salesforce_picklist_values()` e com chamadas repetidas — cache **em
memória**, por `ATTACH`. Use `salesforce_refresh_metadata(catalog[, object])`
para limpar o cache e forçar a próxima chamada a re-descrever o objeto.

#### Erros

Alias de catálogo desconhecido, ou um alias que não é um catálogo Salesforce
→ **erro claro** (sem expor segredos).

#### Para que serve

Permite descobrir, no SQL, quais record types um objeto tem e qual é o
default — útil para mapear dados por record type ou documentar a configuração
do objeto.

#### Uso no dia a dia

```sql
-- record types
SELECT developer_name, label, is_default FROM salesforce_record_types('sf', 'Account');
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

Esta utility é sempre `query` (apenas registros vivos): ela **não** honra a
configuração `sf_query_mode`. Para incluir registros arquivados/excluídos,
escreva a consulta diretamente contra a capacidade `queryAll` do Salesforce.

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

### `salesforce_aggregate(catalog, object, aggregates [, filter [, group_by]])`

#### O que faz

Executa agregados SOQL **server-side** explícitos contra um sObject e
retorna exatamente **uma** linha com o resultado de cada agregação. É a forma
opt-in de pedir um `MIN`/`MAX`/`SUM`/`AVG`/`COUNT` direto ao Salesforce, **sem
baixar as linhas** para o DuckDB.

#### Como funciona

Diferente das funções utilitárias do Nível 4, esta **não** recebe
credenciais: ela **reutiliza a sessão autenticada** de um catálogo já anexado
por `ATTACH`. Você passa o alias desse catálogo no primeiro argumento — sem
reautenticar e sem segredos na chamada.

Todos os argumentos são literais `VARCHAR`:

| Argumento | Obrigatório | Significado |
|---|---|---|
| `catalog` | sim | O alias do `ATTACH` de um org Salesforce já anexado (ex. `'sf'`). A sessão autenticada desse catálogo é reutilizada |
| `object` | sim | O sObject a consultar (ex. `'Account'`); deve ser um identifier válido |
| `aggregates` | sim | Termos de agregação SOQL separados por vírgula (veja abaixo) |
| `filter` | não | Um corpo de `WHERE` SOQL **sem** a palavra `WHERE` (ex. `Industry = 'Technology'`) |
| `group_by` | não | Uma lista de identifiers de campo **simples** separados por vírgula (ex. `Industry` ou `Industry, Type`) para agrupar o resultado |

Internamente, a função monta e executa
`SELECT <aggregates> FROM <object> [WHERE <filter>]` via REST e devolve a
única linha de resultado. Quando você passa `group_by`, ela monta
`SELECT <group_by>, <aggregates> FROM <object> [WHERE <filter>] GROUP BY <group_by>`
e devolve **uma linha por grupo**.

**O argumento `group_by` (5º, opcional).** É uma lista de identifiers de
campo **simples** separados por vírgula — só nomes de campo (ex. `Industry`
ou `Industry, Type`). Não são aceitos dotted/relationship fields, expressões,
nem `ROLLUP` / `CUBE` / `HAVING` neste corte: qualquer um desses é
**rejeitado com erro claro**. O argumento é **posicional depois de `filter`**;
para agrupar **sem** filtro, passe uma string **vazia** em `filter`:

```sql
salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry')
```

**O argumento `aggregates`.** É uma lista de termos separados por vírgula.
Cada termo deve ser uma função de agregação; as permitidas são `MIN`, `MAX`,
`SUM`, `AVG`, `COUNT` e `COUNT_DISTINCT`. Cada termo pode receber um alias no
estilo SOQL (separado por espaço): `MIN(AnnualRevenue) minRev`.

**O modelo de retorno (tudo VARCHAR).** Sem `group_by`, a saída tem
exatamente **uma** linha e **uma coluna por termo**, todas de tipo `VARCHAR`.
A coluna é nomeada pelo alias do termo; quando o termo não tem alias, ela
recebe um nome posicional `expr0`, `expr1`, ... (na ordem dos termos). Como
todo valor volta como texto, é você quem faz o cast no DuckDB (por exemplo
`CAST(n AS BIGINT)` ou `CAST(minRev AS DECIMAL(18,2))`).

**Com `group_by`, a saída tem múltiplas linhas — uma por grupo.** As colunas
de **GROUP vêm primeiro** (cada uma nomeada pelo campo correspondente), e em
seguida vêm as colunas de **aggregate** (alias do termo ou `expr0`,
`expr1`, ...). Tudo continua `VARCHAR`, então o cast segue por sua conta.

**Diagnósticos e modo de leitura.** A função honra a configuração
`sf_query_mode` (`query` / `queryAll`). O SOQL gerado é registrado nos
diagnósticos da sessão, então `salesforce_last_soql()` e
`salesforce_query_cost()` mostram exatamente o que foi enviado.

#### Validação e limitações

- **Só termos de agregação.** Campos "nus" (bare fields, sem função de
  agregação) são **rejeitados** — é isso que garante o contrato de uma única
  linha.
- **`GROUP BY` suportado** via o argumento `group_by`, mas **só com campos
  simples** (identifiers separados por vírgula). Dotted/relationship fields,
  expressões e `ROLLUP` / `CUBE` / `HAVING` continuam **fora de escopo** e são
  rejeitados com erro claro.
- O `object` deve ser um identifier válido; `;` e `SELECT` aninhado são
  rejeitados; os argumentos têm limite de tamanho.
- Agregados de relacionamento/polymorphic são passados direto ao SOQL: se o
  Salesforce recusar, o erro dele aparece **verbatim**.
- As mensagens de erro **nunca** contêm segredos.

#### Não é pushdown transparente

Esta função é **explícita e opt-in**: é você quem decide pedir o agregado
server-side. Ela **não** é um pushdown transparente de `MIN`/`MAX`/`SUM`/`AVG`
— não há OptimizerExtension nem reescrita de plano por trás. Um pushdown
transparente desses agregados exigiria uma OptimizerExtension do DuckDB
(deferido — veja o ROADMAP); enquanto isso, esta função entrega o agregado
server-side sem essa maquinaria. (O `COUNT(*)` continua sendo o único
agregado com pushdown transparente; veja a "Referência de pushdown" abaixo.)

#### Para que serve

Quando você só precisa do agregado — um total, uma média, um mínimo/máximo,
uma contagem distinta — e não quer trazer as linhas para o DuckDB só para
agregá-las localmente. O cálculo acontece no Salesforce e volta em uma única
linha.

#### Uso no dia a dia

```sql
ATTACH 'salesforce://production' AS sf (
  TYPE salesforce,
  client_id     'YOUR_CONSUMER_KEY',
  client_secret 'YOUR_CONSUMER_SECRET',
  refresh_token 'YOUR_REFRESH_TOKEN'
);

SELECT
  CAST(minRev AS DECIMAL(18,2)) AS min_rev,
  CAST(maxRev AS DECIMAL(18,2)) AS max_rev,
  CAST(n AS BIGINT)             AS n
FROM salesforce_aggregate(
  'sf', 'Account',
  'MIN(AnnualRevenue) minRev, MAX(AnnualRevenue) maxRev, COUNT(Id) n',
  'Industry = ''Technology''');
```

Para agrupar **sem** filtro, passe uma string vazia em `filter` e o campo de
agrupamento em `group_by` (note que agora há uma linha por grupo, com a coluna
de GROUP primeiro):

```sql
SELECT Industry, CAST(n AS BIGINT) AS account_count
FROM salesforce_aggregate('sf', 'Account', 'COUNT(Id) n', '', 'Industry')
ORDER BY account_count DESC;
```

E para combinar **filtro + agrupamento**, basta preencher os dois argumentos
(filter não-vazio + group_by):

```sql
SELECT Industry, CAST(n AS BIGINT) AS active_count
FROM salesforce_aggregate(
  'sf', 'Account',
  'COUNT(Id) n',
  'AnnualRevenue > 1000000',
  'Industry')
ORDER BY active_count DESC;
```

## Nível 6 - Report Bridge

Três funções opt-in, somente-leitura, ligam **relatórios** do Salesforce ao
DuckDB. São para descoberta e validação, não extração grande: um relatório é uma
definição feita pelo negócio, não uma query SOQL, e a Reports API síncrona
retorna no máximo 2.000 linhas. Fluxo pretendido: listar um relatório, rodar uma
amostra pequena, reconstruir um SOQL candidato, validar o candidato contra a
amostra, depois materializar em escala via scans normais `sf.<Object>`.

### `salesforce_reports(catalog)`

#### O que faz

Lista **definições** de relatório (não dados) da org anexada.

#### Como funciona

Consulta o sObject `Report` (queryable) com as credenciais do catálogo. Retorna
`Id`, `Name`, `DeveloperName`, `FolderName`, `Format`. Query crua equivalente:
`SELECT Id, Name, DeveloperName, FolderName, Format FROM sf.Report`.

#### Uso no dia a dia

```sql
SELECT Id, Name, Format FROM salesforce_reports('sf') ORDER BY Name;
```

### `salesforce_report(catalog, report_id)`

#### O que faz

Roda um relatório **tabular** de forma síncrona e retorna as linhas como
**amostra** de validação (máx. 2.000 linhas, sem paginação) — NÃO é caminho de
extração grande.

#### Como funciona

- Colunas vêm de `detailColumns`, nomeadas pelo label de
  `reportExtendedMetadata`. Label duplicado, ou que colida com um nome reservado
  `__sf_report_*`, é desambiguado (duplicatas ganham sufixo `_N`; labels com
  prefixo reservado caem para o nome de API).
- Quatro colunas de diagnóstico reservadas são anexadas a cada linha:
  `__sf_report_truncated` (BOOLEAN — o relatório não retornou todos os dados),
  `__sf_report_all_data` (BOOLEAN — o `allData` da API, NULL se ausente),
  `__sf_report_max_rows` (BIGINT — 2000), `__sf_report_guidance` (VARCHAR).
- Relatórios summary/matrix (sem factMap tabular `T!T`) geram erro claro.

> **Ressalva de 0 linhas:** um relatório sem linhas retorna zero linhas e,
> portanto, nenhuma linha de diagnóstico. A truncagem só é observável quando a
> amostra tem ao menos uma linha.

#### Uso no dia a dia

```sql
SELECT * FROM salesforce_report('sf', '00O...');
-- veja __sf_report_truncated para saber se está olhando um resultado completo
```

### `salesforce_report_soql(catalog, report_id)`

#### O que faz

Retorna os ingredientes estruturados do relatório mais um **SOQL candidato
best-effort** — ponto de partida a validar, nunca um contrato de equivalência.

#### Como funciona

Uma linha: `report_id`, `report_name`, `report_type`, `base_object`,
`columns` LIST<VARCHAR>, `filters` LIST<STRUCT(field, op, value)>, `soql`,
`translatable` BOOLEAN, `caveats`.

SOQL não é SQL, então a síntese é conservadora. `soql` é produzido (e
`translatable = true`) só para relatório **tabular** de objeto único cujos
identificadores são nomes Salesforce seguros e cujos filtros usam operadores
suportados (`=`, `!=`, `<`, `>`, e `contains` → `LIKE '%v%'`). Literais string
são aspas-simples e escapados; numéricos ficam crus; valores date/boolean/null,
identificadores inseguros, operadores não suportados, e lógica `OR`/`NOT`/
agrupada em `reportBooleanFilter` definem `translatable = false`, `soql = NULL`,
e explicam o motivo em `caveats`. `base_object` é derivado do report type
(`CustomEntity$X` → `X`) e aceito só quando `X` é um identificador seguro que
**existe e é queryable** no Describe Global. Cada campo projetado/filtrado é então
validado contra o sObject Describe; a projeção do SOQL é relativizada para nomes
de campo simples. Um campo usado em filtro precisa adicionalmente ser
**filterable** (um campo não-filtrável faria o SOQL candidato falhar no
Salesforce). Traversal de relacionamento (`Rel.Field`) e pseudo-colunas não
resolvem. Qualquer falha — base insegura/interna, objeto inexistente ou não
queryable, campo ausente no objeto, cross filter, operador não suportado, ou
lógica `OR`/`NOT`/agrupada — resulta em `translatable = false` com `soql = NULL` e
um caveat explicativo. SOQL parcial nunca é emitido. Os ingredientes estruturados
são sempre retornados, independente de `translatable`.

#### Uso no dia a dia

```sql
SELECT soql, translatable, caveats FROM salesforce_report_soql('sf', '00O...');
-- se translatable, rode o soql via sf.<Object> e compare com a amostra de salesforce_report()
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
