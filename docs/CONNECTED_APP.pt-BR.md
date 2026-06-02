# Connected App + Refresh Token (para o teste de smoke)

Como criar um Connected App e gerar um **refresh token** para rodar a validação
do [SMOKE.md](../SMOKE.md). Use uma org que você está **autorizado a usar**
(sandbox / scratch / Developer Edition, ou a sua própria); a validação live é
manual. Nunca cole nenhum segredo em issue, chat, log ou commit.

> 🇬🇧 English version: [CONNECTED_APP.md](CONNECTED_APP.md)

## Por que não usar usuário + senha?

O fluxo OAuth de usuário-e-senha **não funciona** aqui:

- a extensão só implementa o fluxo de **refresh token** — o `ATTACH` exige
  `client_id` + `client_secret` + `refresh_token` (não existe caminho de
  usuário/senha);
- o [SECURITY.md](../SECURITY.md) proíbe o fluxo usuário-senha (apenas refresh
  token / JWT);
- a Salesforce **desativa** o fluxo OAuth usuário-senha por padrão em orgs
  modernas.

Você vai coletar: `client_id`, `client_secret`, `refresh_token` e o host de
login. `domain=login` (produção/dev) ou `domain=test` (sandbox) corresponde a
`SF_LIVE_LOGIN_URL` = `https://login.salesforce.com` /
`https://test.salesforce.com`.

## 1. Criar o Connected App

Setup (Configuração) → **App Manager** (Gerenciador de Aplicativos) → **New
Connected App** (Novo Aplicativo Conectado):

- **Connected App Name** / **API Name**: ex. `duckdb_salesforce_smoke`
- **Contact Email**: o seu e-mail
- ✅ **Enable OAuth Settings** (Habilitar Configurações OAuth)
- **Callback URL**: `http://localhost:1717/OauthRedirect`
  (qualquer URL que você controle; só é usada no fluxo web-server da §3-B)
- **Selected OAuth Scopes** (Escopos OAuth selecionados): adicione
  - `Manage user data via APIs (api)`
  - `Perform requests at any time (refresh_token, offline_access)`
- ✅ **Require Secret for Web Server Flow** (exige o `client_secret`)
- Opcional, para a forma mais fácil de gerar o token na §3-A: ✅ **Enable Device
  Flow** (Habilitar Fluxo de Dispositivo)
- Salve. **Aguarde ~2–10 minutos** para propagar.

Depois abra o app → **Manage Consumer Details** (Gerenciar Detalhes do
Consumidor) para ver:

- **Consumer Key** (Chave do Consumidor) → `SF_LIVE_CLIENT_ID`
- **Consumer Secret** (Segredo do Consumidor) → `SF_LIVE_CLIENT_SECRET`

## 2. Políticas OAuth + usuário de teste

App → **Manage** (Gerenciar) → **Edit Policies** (Editar Políticas):

- **Permitted Users** (Usuários Permitidos): "Admin approved users are
  pre-authorized" (e atribua seu perfil/permission set) ou "All users may
  self-authorize" para um teste rápido.
- **Refresh Token Policy**: "Refresh token is valid until revoked" (válido até
  ser revogado) ou uma janela que dure mais que o teste.
- **IP Relaxation** (Relaxamento de IP): relaxe se o teste rodar fora das faixas
  de IP confiáveis.

Garanta que o **usuário de teste** tenha **API Enabled** (API habilitada) e
**acesso de leitura ao Account** (Conta).

> Escolha o host de login agora: produção/dev = `https://login.salesforce.com`,
> sandbox = `https://test.salesforce.com`. Use-o em todas as URLs abaixo.

## 3. Gerar o refresh token (escolha UM)

A resposta do token contém segredos — **não** cole em lugar compartilhado.

### 3-A. Device Flow (mais fácil; sem lidar com redirect)

Requer "Enable Device Flow" (§1). Troque `<CID>` e o host conforme necessário.

```sh
# 1) solicitar um device code
curl https://login.salesforce.com/services/oauth2/token \
  -d response_type=device_code -d client_id=<CID> -d scope='api refresh_token'
# -> { "device_code": "...", "user_code": "...", "verification_uri": "...", "interval": 5 }

# 2) abra a verification_uri no navegador, digite o user_code e aprove

# 3) faça polling pelos tokens (repita até parar de retornar authorization_pending)
curl https://login.salesforce.com/services/oauth2/token \
  -d grant_type=device -d client_id=<CID> -d code=<device_code>
# -> { "access_token": "...", "refresh_token": "...", "instance_url": "...", ... }
```

Copie o valor de `refresh_token` (segredo) — veja a §4 para guardar com segurança.

### 3-B. Web-Server Flow (navegador + uma troca)

```sh
# 1) abra esta URL no navegador e aprove; você será redirecionado para
#    <CALLBACK>?code=<AUTH_CODE>  (faça URL-decode do code se necessário)
https://login.salesforce.com/services/oauth2/authorize?response_type=code&client_id=<CID>&redirect_uri=http://localhost:1717/OauthRedirect&scope=api%20refresh_token

# 2) troque o code pelos tokens
curl https://login.salesforce.com/services/oauth2/token \
  -d grant_type=authorization_code -d code=<AUTH_CODE> \
  -d client_id=<CID> -d client_secret=<CSECRET> \
  -d redirect_uri=http://localhost:1717/OauthRedirect
# -> { "access_token": "...", "refresh_token": "...", "instance_url": "...", ... }
```

(Sandbox: troque `login.salesforce.com` por `test.salesforce.com`.)

## 4. Guardar os valores como variáveis de ambiente (sem echo, sem histórico)

PowerShell 7 — mascarado, não salvo no histórico do shell:

```powershell
$env:SF_LIVE_CLIENT_ID     = Read-Host 'SF_LIVE_CLIENT_ID'
$env:SF_LIVE_CLIENT_SECRET = Read-Host 'SF_LIVE_CLIENT_SECRET' -MaskInput
$env:SF_LIVE_REFRESH_TOKEN = Read-Host 'SF_LIVE_REFRESH_TOKEN' -MaskInput
$env:SF_LIVE_LOGIN_URL     = 'https://login.salesforce.com'   # ou https://test.salesforce.com
```

Conferir (apenas os nomes): `Get-ChildItem Env:SF_LIVE_* | Select-Object Name`.

Depois siga o [SMOKE.md](../SMOKE.md) §3/§4 para rodar a validação. Limpe ao
final: `Remove-Item Env:SF_LIVE_*`.

## 5. Segurança

- Nunca faça commit de `.env`, saída do curl, transcript do terminal ou
  qualquer coisa com token. Não cole `client_secret` / `refresh_token` /
  `access_token` em issues, PRs, chat ou logs.
- Se algum valor vazar, **revogue**: Setup → Connected Apps OAuth Usage →
  revogar, ou redefina o Consumer Secret, e gere de novo.
- **C.5:** nada disso autoriza qualquer ação no `duckdb/community-extensions`.
