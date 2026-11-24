# Guia passo a passo: montar, testar e tirar os prints

> **Prazo: 24/11.** Sigam na ordem. Cada 📸 é um print, com o **nome do arquivo** que ele deve ter
> e **onde ele vai no caderno**. Salvem todos em `docs/prints/`.
>
> ⛔ **Regra de ouro dos prints:** nenhum print pode mostrar a aba `config.h`, o campo da chave HMAC
> com o texto visível nem a tela de Secrets com o valor. Antes de cada print, **confiram se a aba
> aberta no Wokwi é a `sketch.ino`**. Se vazar, troquem a chave (`scripts\gerar_chaves.ps1`).

Tempo estimado: **2h30 a 3h**, sem contar o caderno.

---

## Parte A: preparar a nuvem (Cloudflare, sem wrangler) · ~25 min

Vocês já fizeram isso na Estação Cofre pelo painel web da Cloudflare. Vamos criar um **Worker novo**
(`silo-graos`) para não quebrar o da atividade anterior, que pode ainda ser avaliado.

### A1. Banco D1 (pode reusar o da Estação Cofre)
1. Acessem <https://dash.cloudflare.com> → **Storage & Databases** → **D1 SQL Database**.
2. Abram o banco que vocês já usam (o que tem a tabela `leituras`) → aba **Console**.
3. Colem **todo** o conteúdo de `worker/schema.sql` e cliquem em **Execute**.
4. Confiram na aba **Tables** que apareceu `silo_leituras`.

### A2. Worker
1. **Workers & Pages** → **Create** → **Create Worker** → nome: `silo-graos` → **Deploy** (com o Hello World mesmo).
2. **Edit code** → apaguem tudo → colem `worker/worker.js` → **Deploy**.
3. **Settings → Bindings → Add → D1 database**: nome da variável **`DB`**, banco: o do passo A1.
4. **Settings → Variables and Secrets → Add**:
   - Tipo **Secret**, nome `API_KEY`, valor: o mesmo `API_KEY` do `config.h` da Estação Cofre (ou um novo, gerado pelo `scripts\gerar_chaves.ps1`)
   - Tipo **Secret**, nome `CHAVE_HMAC`, valor: idem
   - Tipo **Text** (variable), nome `EXIGIR_ASSINATURA`, valor `sim`
5. Anotem a URL: `https://silo-graos.SEU-USUARIO.workers.dev`.
6. Teste rápido: abram `SUA-URL/resumo` no navegador. Deve aparecer `[]` (lista vazia). ✅

### A3. Certificados (CA do Worker novo)
O domínio `*.workers.dev` é o mesmo, então o `certificados.h` que já veio na pasta deve funcionar.
Se o HTTPS der `X509 - Certificate verification failed`, rodem (PowerShell, dentro de `scripts`):
```powershell
powershell -ExecutionPolicy Bypass -File .\gerar_certificados.ps1 -Worker https://silo-graos.marcelotellesest.workers.dev/
```

---

## Parte B: montar no Wokwi · ~20 min

1. Acessem <https://wokwi.com> → **New Project** → **ESP32**.
2. Na aba **`diagram.json`**: apaguem tudo e colem `firmware/silo_graos/diagram.json`.
   Vão aparecer: ESP32, DHT22, módulo relé, LED azul (VENTILADOR), potenciômetro (BATERIA) e a chave (SEM INTERNET).
3. Na aba **`sketch.ino`**: colem `firmware/silo_graos/silo_graos.ino`.
4. Clique na setinha ao lado das abas → **New file…** e criem as três abas:
   - `libraries.txt` → colem `firmware/silo_graos/libraries.txt`
   - `certificados.h` → colem `firmware/silo_graos/certificados.h`
   - `config.h` → copiem `config.example.h` e preencham `WORKER_URL`, `API_KEY` e `CHAVE_HMAC` (os mesmos do Worker)
5. **Não cliquem em Share/Save público** com o `config.h` preenchido (buraco 5). Se precisarem salvar, salvem como privado.
6. ▶ **Start**. No começo da simulação:
   - **Potenciômetro BATERIA**: começa em bateria cheia (~100 %). Girar para a direita **descarrega** (abaixo de 20 % ele entra em modo economia e manda o alerta de bateria).
   - **Chave SEM INTERNET**: começa com internet. Se o Serial mostrar `[REDE] chave de teste em SEM INTERNET`, mudem a chave de lado.

> 💡 O ESP32 **dorme 30 s** entre os ciclos (modo demo). A cada ciclo o Serial mostra `===== Silo silo1 | ciclo N =====`.
> No Wokwi um ciclo acordado leva ~20 a 30 s porque o TLS simulado é lento. É normal.

📸 **PRINT 01**: `p01_circuito_wokwi.png`
- **Onde:** tela do Wokwi mostrando **o circuito inteiro** (sem o código).
- **Vai em:** Etapa 9 (“Desenho ou print do sistema”) e, opcionalmente, Etapa 2.

📸 **PRINT 02**: `p02_serial_ciclo_completo.png`
- **Onde:** Serial Monitor com **um ciclo inteiro**, de `===== ... ciclo N` até `[SONO] ... dormindo 30 s`.
  Tem que aparecer: `[DHT]`, `[REGRA]`, `[WiFi] ok`, `[NTP] ok`, `[MQTT] ... ok`, `[MQTT] telemetria ok`, `[HTTPS] 1 leitura(s) -> 200`, `[ENERGIA]`.
- **Vai em:** Etapa 3 (regra) e Etapa 10 (Isolado).
- ⚠️ Confiram que a linha `[HTTPS]` **não** mostra chave (ela mostra só o código de retorno).

---

## Parte C: painel · ~5 min

1. Abram `painel/painel.html` com dois cliques (abre no navegador).
2. Preencham: `marcelo` / `silo1` / URL do Worker → **Conectar**.
3. Colem a `CHAVE_HMAC` no campo do comando (ele é do tipo senha, então aparece como ••••).
4. No card **Histórico**, colem a `API_KEY` no campo da **ponte do histórico** e deixem a caixa marcada.

> ⚠️ **Por que a ponte existe:** em PC mais lento, o ESP32 simulado leva ~13 s no handshake TLS com a Cloudflare
> (certificado ECDSA P-384), e a Cloudflare fecha a conexão antes (`SSL - The connection indicated an EOF`).
> Quando isso acontece, o ESP32 publica o lote **já assinado com HMAC** em `…/ponte`, o painel repassa ao Worker
> e confirma em `…/ponte_ok`. O painel **não consegue alterar** nada: se mudar 1 byte, o Worker recusa (401).
> No Serial aparece `[PONTE] painel confirmou: Worker gravou N leitura(s)`. **Para isso o painel precisa estar aberto.**
>
> 🔁 **Atualizem o Worker:** o `worker.js` mudou (CORS para o POST do painel). Colem de novo no editor da Cloudflare e cliquem em **Deploy**.
4. Esperem 1 ou 2 ciclos: aparecem o estado, a última leitura, o gráfico e o resumo.

📸 **PRINT 08**: `p08_painel_completo.png`
- **Onde:** o navegador com **o painel inteiro** (diminuam o zoom com Ctrl - se precisar), mostrando estado, leitura, gráfico com algumas leituras e a tabela de resumo.
- **Vai em:** Etapa 6 (“O que o painel mostra”).

---

## Parte D: testes da regra (Etapas 1, 2, 3 e 10) · ~30 min

Para mudar a temperatura: **cliquem no DHT22 no Wokwi** e arrastem o controle de temperatura ou umidade.

### D1. RF1: liga com confirmação (2 leituras)
1. DHT em **32 °C** e umidade em **60 %**.
2. Esperem o próximo ciclo: `[REGRA] ... acima do limite, confirmando (1/2)`.
3. No ciclo seguinte: `[REGRA] ventilador LIGADO | ... liga: acima do limite em 2 leituras`, e o LED azul acende.

📸 **PRINT 03**: `p03_rf1_liga_confirmado.png`
- **Onde:** Serial com as duas linhas `[REGRA]` (1/2 e liga) **+ o LED azul aceso** na mesma tela.
- **Vai em:** Etapa 1, RF1.

### D2. RF2: histerese e tempo mínimo
1. Com o ventilador ligado, ponham **28 °C**: `mantem ligado: ainda acima do limite de desligar`.
2. Ponham **26 °C** e umidade **60 %**: se ainda não passaram 60 s, `mantem ligado: tempo minimo`; depois, `desliga: voltou abaixo de 27 C e 70 %`, e o LED apaga.

📸 **PRINT 04**: `p04_rf2_histerese.png`
- **Onde:** Serial mostrando a sequência `mantem ligado` → `desliga`.
- **Vai em:** Etapa 1, RF2, e Etapa 10 (No limite).

### D3. Falha do sensor (estado seguro)
1. Com o ventilador **ligado** (repitam o D1), **cliquem no fio verde do DHT (SDA)** e apertem **Delete**.
2. No ciclo seguinte: `[DHT] FALHA NA LEITURA (NaN)` e `[REGRA] ventilador desligado | ... sensor_falha: estado seguro (desligado)`.
3. No 2º ciclo com falha: `[ALERTA] enviado: {"tipo":"sensor_falha"...}`.
4. Para voltar: **Ctrl+Z** no diagrama (ou recolem o `diagram.json`).

📸 **PRINT 05**: `p05_falha_sensor_estado_seguro.png`
- **Onde:** Serial com NaN + estado seguro + o alerta.
- **Vai em:** Etapa 2 (coluna “Se falhar”) e Etapa 10 (Na falha).

### D4. Internet caiu e voltou
1. Movam a **chave SEM INTERNET** até o Serial mostrar `[REDE] chave de teste em SEM INTERNET | 1 leitura(s) guardada(s)`.
2. Esperem **3 ciclos**: o número sobe (1 → 2 → 3) e o ventilador continua sendo decidido normalmente.
3. Voltem a chave. No ciclo seguinte: `[HTTPS] 4 leitura(s) -> 200 OK: 4 leitura(s) de silo1`.

📸 **PRINT 06**: `p06_sem_internet_buffer.png`
- **Onde:** Serial com o buffer subindo **e** o envio do lote com 200 (se não couber, façam dois prints: `p06a` e `p06b`).
- **Vai em:** Etapa 3 (“Se a internet cair?”) e Etapa 10 (Na falha).

### D5. Reinício
1. Cliquem em **Stop** e depois em **Start** no Wokwi (é como tirar e ligar o USB).
2. O Serial mostra `ciclo 1 | despertar: liga/reset`.

📸 **PRINT 19**: `p19_reinicio.png`
- **Onde:** Serial com `ciclo 1 | despertar: liga/reset` e o primeiro `[REGRA]`.
- **Vai em:** Etapa 10 (No reinício).

---

## Parte E: comando, ack e alerta (Etapa 6) · ~20 min

### E1. Comando com o ESP32 dormindo
1. Esperem o painel mostrar **DORMINDO (deep sleep)** e “Acorda às HH:MM:SS” (no Wokwi o sono é simulado, mas para o painel é igual: o ESP32 está fora do broker).
2. Cliquem em **Ligar ventilador**.
3. O painel mostra **AGUARDANDO … ESP32 dormindo, será executado quando acordar**.

📸 **PRINT 10**: `p10_comando_aguardando_dormindo.png`
- **Onde:** os cards de **Estado** (DORMINDO) e de **Comando** (AGUARDANDO) juntos.
- **Vai em:** Etapa 6 (“Como o painel sabe…”).

### E2. Comando executado
1. Quando o ESP32 acordar, o painel muda para **✔ EXECUTADO OK: vent:ON (manual por 5 min)**.
2. No Serial aparece `[CMD] OK: vent:ON ...` e o LED azul acende.

📸 **PRINT 11**: `p11_comando_executado.png`
- **Onde:** de preferência **lado a lado**: painel com ✔ EXECUTADO + Wokwi com o LED aceso e o `[CMD] OK` no Serial.
- **Vai em:** Etapa 1 (RF3) e Etapa 6.

Depois cliquem em **Automático** para voltar ao modo normal.

### E3. Alerta com cooldown
1. DHT em **36 °C**.
2. No próximo ciclo: `[ALERTA] enviado: {"tipo":"temp_critica"...}`, e o painel mostra o alerta e apita.
3. No ciclo seguinte: `[ALERTA] temp_critica em cooldown (faltam N s)`, e **nenhum** alerta novo aparece no painel.

📸 **PRINT 12**: `p12_alerta_cooldown.png`
- **Onde:** card **Alertas** do painel + Serial com `enviado` e depois `em cooldown`.
- **Vai em:** Etapa 1 (RF4) e Etapa 6 (Alerta).

---

## Parte F: segurança (Etapa 7) · ~20 min

### F1. Comando falso (sem assinatura)
1. No painel, **desmarquem “assinar”** e cliquem em **Desligar**.
2. Quando o ESP32 acordar: **✘ RECUSADO: sem assinatura**.

📸 **PRINT 13**: `p13_ataque_sem_assinatura.png`. Vai em: Etapa 7 (buraco 4) e Etapa 10 (No ataque).

### F2. Replay
1. **Marquem “assinar” de novo**, mandem **Ligar ventilador** e esperem o ✔ EXECUTADO.
2. Cliquem em **Reenviar o último (replay)**.
3. Resultado: **✘ RECUSADO: replay (comando repetido)**.

📸 **PRINT 14**: `p14_ataque_replay.png`. Vai em: Etapa 7 (buraco 4).

### F3. Ataque no Worker
No PowerShell, dentro da pasta `scripts`:
```powershell
powershell -ExecutionPolicy Bypass -File .\ataque_worker.ps1 -Url https://silo-graos.SEU-USUARIO.workers.dev
```
Os dois ataques devem sair em **verde, com 401**.

📸 **PRINT 15**: `p15_ataque_worker_401.png`. Vai em: Etapa 7 (buraco 3) e Etapa 10 (No ataque).

### F4. TLS de verdade (CA errada)
1. No `sketch.ino`, dentro de `conectarMqtt()`, troquem **temporariamente**
   `tlsMqtt.setCACert(CA_MOSQUITTO);` por `tlsMqtt.setCACert(CA_WORKER);`.
2. Rodem: `[MQTT] ... falhou rc=-2 | TLS: X509 - Certificate verification failed`.
3. **Voltem para `CA_MOSQUITTO`** depois do print!

📸 **PRINT 07**: `p07_tls_ca_errada.png`. Vai em: Etapa 4 (TLS) e Etapa 7 (buracos 1 e 2).

### F5. Nenhum segredo no GitHub
Depois de subir o repositório (Parte H): print da **lista de arquivos do GitHub** mostrando a pasta
`firmware/silo_graos/` **sem** `config.h`. Pode ser junto com a tela do Worker em
**Settings → Variables and Secrets**, que mostra só os **nomes** (os valores ficam ocultos).

📸 **PRINT 16**: `p16_sem_segredo_github.png`. Vai em: Etapa 7 (buraco 5).

---

## Parte G: histórico, energia e duração (Etapas 5, 8 e 10) · ~15 min

### G1. Consulta de resumo no D1
1. Cloudflare → D1 → seu banco → **Console**.
2. Colem a consulta comentada no fim do `schema.sql` (tirem os `--` do começo das linhas) → **Execute**.

📸 **PRINT 09**: `p09_d1_resumo.png`. Vai em: Etapa 5 (Consulta de resumo).

### G2. Energia
Peguem no Serial as 3 linhas `[ENERGIA]` de um ciclo **depois do ciclo 5** (a média já está estável).

📸 **PRINT 17**: `p17_energia_serial.png`. Vai em: Etapa 8 (Evidência).
- ✏️ **No caderno, Etapa 8, item 2:** troquem “≈ 30 s” e as contas pela **média** que aparecer na linha `media X s`.
  Conta: `(160 × X + 0,15 × 600) ÷ (X + 600)`, e depois `4800 ÷ resultado`. A própria linha `produto (dorme 600 s)` já traz esse resultado pronto.

### G3. Duração de 10 min
Deixem tudo rodando 10 min (com o painel aberto). Depois:

📸 **PRINT 18**: `p18_duracao_10min.png`
- **Onde:** Serial mostrando `ciclo 15` ou mais **+** o gráfico do painel com a linha contínua de 10 min.
- **Vai em:** Etapa 10 (Na duração).

---

## Parte H: GitHub e entrega · ~20 min

1. Criem um repositório novo no GitHub (ex.: `iot-n2-silo-graos`).
2. No PowerShell, dentro da pasta `IoT_N2_Silo_Graos`:
```powershell
git init
git add .
git status
```
3. ⛔ **Confiram no `git status` que `firmware/silo_graos/config.h` NÃO aparece.** (Ele só existe no Wokwi; se vocês criarem um aqui, o `.gitignore` já o protege.)
4. Depois:
```powershell
git commit -m "Trabalho N2 - silo de graos"
git branch -M main
git remote add origin https://github.com/MCTelles/iot-n2-silo-graos.git
git push -u origin main
```
5. Abram o caderno `docs/Caderno_N2_Silo_Graos.docx` e:
   - cada caixa amarela **“COLE AQUI O PRINT NN”** → apaguem o texto da caixa e colem a imagem (Inserir → Imagens);
   - cada texto **vermelho em itálico** (conferir/preencher) → ajustem e apaguem o aviso;
   - preencham o **link do repositório** e o **sobrenome do Milton**.
6. Salvem também como PDF (Arquivo → Salvar como → PDF), coloquem em `docs/` e façam `git add`, `git commit` e `git push` de novo.
7. Gravem um **vídeo curto** (Win+Alt+R grava a tela no Windows), de 1 a 2 min, mostrando o E1 → E2 e o F1. É o plano B se o Wokwi travar na apresentação.
8. Entreguem no Classroom: o caderno (.docx ou .pdf) + o link do repositório.

---

## Tabela resumo dos prints

| # | Arquivo | O que mostra | Etapa do caderno |
|---|---|---|---|
| 01 | p01_circuito_wokwi | circuito inteiro | 9 (e 2) |
| 02 | p02_serial_ciclo_completo | um ciclo do Serial | 3, 10 |
| 03 | p03_rf1_liga_confirmado | confirmando 1/2 → liga + LED | 1 (RF1) |
| 04 | p04_rf2_histerese | mantém → desliga | 1 (RF2), 10 |
| 05 | p05_falha_sensor_estado_seguro | NaN → desligado + alerta | 2, 10 |
| 06 | p06_sem_internet_buffer | buffer → lote 200 | 3, 10 |
| 07 | p07_tls_ca_errada | X509 failed | 4, 7 |
| 08 | p08_painel_completo | painel | 6 |
| 09 | p09_d1_resumo | SQL no console D1 | 5 |
| 10 | p10_comando_aguardando_dormindo | AGUARDANDO + DORMINDO | 6 |
| 11 | p11_comando_executado | ✔ EXECUTADO + LED | 1 (RF3), 6 |
| 12 | p12_alerta_cooldown | alerta + cooldown | 1 (RF4), 6 |
| 13 | p13_ataque_sem_assinatura | RECUSADO sem assinatura | 7, 10 |
| 14 | p14_ataque_replay | RECUSADO replay | 7 |
| 15 | p15_ataque_worker_401 | 401 ×2 | 7, 10 |
| 16 | p16_sem_segredo_github | repo sem config.h | 7 |
| 17 | p17_energia_serial | [ENERGIA] | 8 |
| 18 | p18_duracao_10min | ciclo ≥ 15 + gráfico | 10 |
| 19 | p19_reinicio | ciclo 1 liga/reset | 10 |

---

## Quando der erro

| Sintoma | Causa provável | O que fazer |
|---|---|---|
| `[HTTPS] ... -> 401 Recusado: expirada` | relógio do Wokwi atrasado | é raro (o NTP roda a cada despertar); a leitura fica no buffer e sobe no próximo ciclo |
| `[HTTPS] ... -> 401 assinatura invalida` | `CHAVE_HMAC` diferente entre o `config.h` e o Secret | colem de novo os dois (cuidado com espaço no fim) |
| `[HTTPS] -> 500 Configure o Secret` | nome do Secret errado | tem que ser exatamente `API_KEY` e `CHAVE_HMAC` |
| `[HTTPS] -1 ... X509` | CA do Worker | rodem o `gerar_certificados.ps1` (Parte A3) |
| `[MQTT] falhou rc=5` | o broker recusou o login | deixem `MQTT_USER ""` e `MQTT_PASS ""` |
| painel: “erro ao buscar” | URL do Worker errada ou com `/` no fim | conferir; testar `URL/resumo` no navegador |
| painel não mostra nada | grupo/local diferente do `config.h` | os dois têm que ser `marcelo` / `silo1` |
| bateria 0 % e alerta de bateria | potenciômetro girado para a direita | girem para a esquerda |
| sempre “SEM INTERNET” | a chave está na posição de teste | mudem a chave de lado |
| `[SONO] ... (sono simulado no Wokwi)` | é o esperado no modo demo | o Wokwi não consegue sair do deep sleep (reinicia pelo watchdog, `rst:0x8 TG1WDT_SYS_RESET`). No demo o sono é Wi-Fi desligado + espera; com `MODO_DEMO 0` (ESP32 real) é deep sleep de verdade. Citem isso se perguntarem |
| `[HTTPS] ... -1` + `SSL - The connection indicated an EOF` (~13 s) | o ESP32 simulado é lento demais para o handshake com a Cloudflare | normal em PC lento: o lote vai pela **ponte**. Deixem o painel aberto, com a API key preenchida |
| `[PONTE] sem confirmacao (painel fechado?)` | painel fechado, sem a API key, ou Worker sem o CORS novo | abram o painel, colem a API key e refaçam o Deploy do `worker.js` |
| painel: `✘ Worker 401` na ponte | `API_KEY` do painel diferente da do Worker | colem a mesma do Secret |
| `rst:0x8 (TG1WDT_SYS_RESET)` em loop e `ciclo 1` sempre | sketch antigo (deep sleep real no Wokwi) | colem o `silo_graos.ino` atualizado |
| Wokwi muito lento ou travando | simulação pesada | fechem outras abas; se não der, usem o vídeo |
