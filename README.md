# SiloSentinela: aeração automática de silo de grãos (IoT · Trabalho N2)

**Marcelo e Milton · Turma AMF 2026-02 · Prof. Leonam V. Hemann**

## O que o sistema faz
Um ESP32 dentro do silo mede **temperatura e umidade** (DHT22) e **liga o ventilador de aeração** (relé)
quando o grão corre risco: **T ≥ 30 °C ou UR ≥ 75 %** em 2 leituras seguidas. Ele só desliga abaixo de
**27 °C e 70 %** e depois de um tempo mínimo (histerese). Como **não há tomada no silo**, ele roda na
bateria e **dorme (deep sleep)** entre as leituras, com autonomia estimada em ~89 dias.

- **Decide no ESP32** (funciona sem internet; guarda até 48 leituras e envia quando a rede volta).
- **MQTT com TLS** (test.mosquitto.org:8885): telemetria, estado (retido + LWT), comando (retido e assinado), ack e alerta.
- **Cloudflare Worker + D1**: histórico com consulta de resumo por hora.
- **Painel** (`painel/painel.html`): tempo real por wss, gráfico, resumo, comando com confirmação e alertas com cooldown.

## Estrutura
```
firmware/silo_graos/   silo_graos.ino, diagram.json (Wokwi), libraries.txt,
                       certificados.h (CAs públicas), config.example.h
worker/                worker.js, schema.sql
painel/                painel.html
scripts/               gerar_chaves.ps1, gerar_certificados.ps1, ataque_worker.ps1
docs/                  Caderno_N2_Silo_Graos.docx, prints/
GUIA_PASSO_A_PASSO.md  montagem, testes e prints, passo a passo
```

## Como montar
1. **D1:** rode `worker/schema.sql` no console do banco.
2. **Worker:** cole `worker/worker.js`; binding D1 `DB`; Secrets `API_KEY` e `CHAVE_HMAC`; variável `EXIGIR_ASSINATURA=sim`.
3. **Wokwi:** novo projeto ESP32 → cole `diagram.json`, `silo_graos.ino`, `libraries.txt` e `certificados.h`;
   crie `config.h` a partir do `config.example.h`.
4. **Painel:** abra `painel/painel.html`, informe grupo, local e URL do Worker, cole a chave HMAC e clique em **Conectar**.

O detalhe de cada passo está no [GUIA_PASSO_A_PASSO.md](GUIA_PASSO_A_PASSO.md).

## Como testar
| Teste | Como |
|---|---|
| Liga / histerese | DHT 32 °C (liga no 2º ciclo) → 28 °C (mantém) → 26 °C (desliga) |
| Falha do sensor | apagar o fio SDA → ventilador desligado + alerta |
| Sem internet | chave SEM INTERNET → buffer cresce → volta → lote com 200 |
| Comando | painel → AGUARDANDO (dormindo) → ✔ EXECUTADO |
| Ataque | sem assinatura / replay → RECUSADO; `scripts\ataque_worker.ps1` → 401 |

## Segurança: camada × ameaça
- **TLS com CA conferida** (`setCACert`) no MQTT (8885) e no HTTPS; nenhum `setInsecure()`: ninguém se passa pelo broker ou pelo Worker.
- **API key + HMAC do corpo + ts ≤ 60 s + assinatura de uso único** no `/insert`: ninguém grava leitura falsa nem repete uma requisição capturada.
- **Comando assinado** (`comando|ts|HMAC`) com anti-replay na memória RTC: ninguém desliga a aeração pelo tópico público.
- **Segredos** só no `config.h` (no `.gitignore`) e nos Secrets do Worker.
