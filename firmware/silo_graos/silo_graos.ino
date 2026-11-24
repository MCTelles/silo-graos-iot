// ================================================================
//  SILO DE GRAOS - aeracao automatica com bateria (Trabalho N2 - IoT)
//
//  O que faz, a cada vez que acorda:
//    1. Le temperatura e umidade dentro do silo (DHT22/DHT11)
//    2. DECIDE AQUI MESMO (na borda) se liga o ventilador de aeracao
//       -> funciona mesmo sem internet
//    3. Se tiver rede: publica no MQTT (TLS), recebe comandos assinados,
//       manda o historico pro Worker (HTTPS + API key + HMAC)
//    4. Volta a dormir (deep sleep) para a bateria durar
//
//  Herdado da Estacao Cofre:
//    Camada 1  TLS de verdade ......... setCACert() no MQTT e no HTTPS
//    Camada 2  API key ................ cabecalho X-API-Key no Worker
//    Camada 3  Assinatura HMAC + ts ... cabecalho X-Assinatura (anti-replay)
//    Bonus     Comandos assinados ..... "comando|ts|assinatura"
//
//  Arquivos desta pasta:
//    config.h        -> SEGREDOS (copie de config.example.h). Nao versionar!
//    certificados.h  -> CAs publicas (gerado por scripts/gerar_certificados.ps1)
//
//  Bibliotecas: PubSubClient, DHT sensor library, Adafruit Unified Sensor.
// ================================================================
#include <WiFi.h>
#include <WiFiClientSecure.h>
#include <HTTPClient.h>
#include <PubSubClient.h>
#include <DHT.h>
#include <time.h>
#include "driver/gpio.h"
#include "esp_sleep.h"
#include "mbedtls/md.h"
#include "config.h"
#include "certificados.h"

#ifndef DHTTYPE
#define DHTTYPE DHT11
#endif
#ifndef MODO_DEMO
#define MODO_DEMO 1
#endif

// ---------------- Pinos ----------------
#define PINO_DHT      4    // DHT22: dado
#define PINO_RELE     26   // rele do ventilador de aeracao
#define PINO_LED      2    // LED que repete o estado do ventilador (facil de ver)
#define PINO_BATERIA  34   // potenciometro que SIMULA a tensao da bateria
#define PINO_SEM_NET  27   // chave de teste: em LOW simula "internet caiu" (Etapa 10)

// ---------------- Regra de decisao (histerese) ----------------
#define T_LIGA_C        30.0   // liga se temperatura >= 30 C ...
#define U_LIGA_PCT      75.0   // ... ou umidade >= 75 %
#define T_DESLIGA_C     27.0   // desliga so quando temperatura <= 27 C ...
#define U_DESLIGA_PCT   70.0   // ... E umidade <= 70 %
#define T_CRITICA_C     35.0   // alerta: risco de fungo / combustao
#define CONFIRMACOES    2      // precisa passar do limite em 2 leituras seguidas

// ---------------- Tempos ----------------
#if MODO_DEMO
  #define DORME_S          30      // demonstracao no Wokwi
  #define MIN_LIGADO_S     60      // tempo minimo com o ventilador ligado
  #define MANUAL_DURA_S    300     // comando manual vale 5 min, depois volta ao automatico
  #define COOLDOWN_ALERTA_S 120    // mesmo alerta nao se repete antes de 2 min
#else
  #define DORME_S          600     // 10 min
  #define MIN_LIGADO_S     900     // 15 min
  #define MANUAL_DURA_S    3600    // 1 h
  #define COOLDOWN_ALERTA_S 1800   // 30 min
#endif
#define DORME_ECONOMIA_S   (DORME_S * 3)          // bateria fraca: acorda 3x menos
#define JANELA_CMD_S       (DORME_S * 3 + 120)    // validade de um comando (cobre o sono)
#define BATERIA_FRACA_PCT  20

// gpio_hold mantem o rele ligado durante o deep sleep no ESP32 REAL.
// No Wokwi ele faz o chip reiniciar pelo watchdog (rst:0x8 TG1WDT) e apaga a
// memoria RTC; por isso fica desligado no modo demo. O estado do rele e'
// reaplicado a cada despertar de qualquer jeito.
#ifndef SEGURAR_RELE_NO_SONO
  #define SEGURAR_RELE_NO_SONO (!MODO_DEMO)
#endif

// ---------------- Estimativa de energia (Etapa 8) ----------------
#define I_ACORDADO_MA   160.0   // ESP32 com Wi-Fi ligado (media)
#define I_DORMINDO_MA   0.15    // deep sleep + DHT em repouso + regulador
#define BATERIA_MAH     6000.0  // 2 x 18650 de 3000 mAh em paralelo
#define BATERIA_UTIL    0.8     // so 80 % e' aproveitavel
#define DORME_PRODUTO_S 600     // a conta e' feita para o intervalo do produto

// ================================================================
//  Memoria RTC: sobrevive ao deep sleep (a RAM comum e' apagada)
// ================================================================
struct Leitura { uint32_t ts; int16_t t10; uint16_t u10; uint8_t vent; uint8_t manual; };
#define BUF_MAX 48                           // 48 leituras = 24 min (demo) ou 8 h (produto) sem internet

RTC_DATA_ATTR uint32_t ciclos = 0;
RTC_DATA_ATTR uint64_t msAcordadoTotal = 0;
RTC_DATA_ATTR bool     ventLigado = false;
RTC_DATA_ATTR uint32_t ligouEm = 0;
RTC_DATA_ATTR uint8_t  acimaSeguidas = 0;
RTC_DATA_ATTR uint8_t  falhasSensor = 0;
RTC_DATA_ATTR bool     modoManual = false;
RTC_DATA_ATTR uint32_t manualAte = 0;
RTC_DATA_ATTR uint32_t ultimoTsCmd = 0;      // anti-replay dos comandos
RTC_DATA_ATTR uint32_t ultAlertaTemp = 0;    // cooldown por tipo de alerta
RTC_DATA_ATTR uint32_t ultAlertaSensor = 0;
RTC_DATA_ATTR uint32_t ultAlertaBateria = 0;
RTC_DATA_ATTR Leitura  buf[BUF_MAX];
RTC_DATA_ATTR uint8_t  nBuf = 0;
RTC_DATA_ATTR uint32_t perdidas = 0;         // leituras descartadas com buffer cheio

// ---------------- Objetos ----------------
DHT dht(PINO_DHT, DHTTYPE);
WiFiClientSecure tlsMqtt;
PubSubClient mqtt(tlsMqtt);

String BASE      = String("sis1a/") + GRUPO + "/" + LOCAL + "/";
String T_TELEM   = BASE + "telemetria";
String T_ESTADO  = BASE + "estado";
String T_CMD     = BASE + "comando";
String T_ACK     = BASE + "ack";
String T_ALERTA  = BASE + "alerta";
String T_PONTE   = BASE + "ponte";      // lote assinado (plano B do historico)
String T_PONTE_OK = BASE + "ponte_ok";  // painel confirma que o Worker gravou

uint32_t tsPonteEsperado = 0;
bool     ponteConfirmada = false;

float  tempC = NAN, umidPct = NAN;
int    bateriaPct = 100;
String motivo = "";
bool   temRede = false;
bool   wifiIniciado = false;

uint32_t agora() { return (uint32_t)time(nullptr); }
bool relogioOk() { return agora() > 1700000000UL; }

// ================================================================
//  HMAC (igual a Estacao Cofre)
// ================================================================
String hmacHex(const String& msg) {
  uint8_t mac[32];
  mbedtls_md_hmac(mbedtls_md_info_from_type(MBEDTLS_MD_SHA256),
                  (const uint8_t*)CHAVE_HMAC, strlen(CHAVE_HMAC),
                  (const uint8_t*)msg.c_str(), msg.length(), mac);
  char hex[65];
  for (int i = 0; i < 32; i++) sprintf(hex + 2 * i, "%02x", mac[i]);
  hex[64] = '\0';
  return String(hex);
}

bool iguais(const String& a, const String& b) {
  if (a.length() != b.length()) return false;
  uint8_t dif = 0;
  for (unsigned int i = 0; i < a.length(); i++) dif |= a[i] ^ b[i];
  return dif == 0;
}

// ================================================================
//  Atuador: o rele precisa CONTINUAR no mesmo estado enquanto o ESP32 dorme.
//  gpio_hold "congela" o pino durante o deep sleep.
// ================================================================
void aplicarVentilador() {
  gpio_hold_dis((gpio_num_t)PINO_RELE);
  gpio_hold_dis((gpio_num_t)PINO_LED);
  digitalWrite(PINO_RELE, ventLigado ? HIGH : LOW);
  digitalWrite(PINO_LED,  ventLigado ? HIGH : LOW);
}

// ================================================================
//  REGRA DE DECISAO (roda no ESP32 = borda). Escreve o motivo em "motivo".
// ================================================================
void decidir(uint32_t ts) {
  // 1) Sensor falhou -> ESTADO SEGURO: ventilador DESLIGADO.
  //    Ventilar "as cegas" pode soprar ar umido (chuva, noite) no grao e gasta bateria.
  if (isnan(tempC) || isnan(umidPct)) {
    falhasSensor++;
    ventLigado = false;
    acimaSeguidas = 0;
    motivo = "sensor_falha: estado seguro (desligado)";
    return;
  }
  falhasSensor = 0;

  // 2) Comando manual tem prioridade, mas expira sozinho
  if (modoManual) {
    if (relogioOk() && ts >= manualAte) {
      modoManual = false;
      Serial.println("[REGRA] manual expirou -> volta ao automatico");
    } else {
      motivo = "manual (painel)";
      return;
    }
  }

  bool pedeLigar    = (tempC >= T_LIGA_C) || (umidPct >= U_LIGA_PCT);
  bool podeDesligar = (tempC <= T_DESLIGA_C) && (umidPct <= U_DESLIGA_PCT);

  if (!ventLigado) {
    if (pedeLigar) {
      acimaSeguidas++;
      if (acimaSeguidas >= CONFIRMACOES) {
        ventLigado = true; ligouEm = ts; acimaSeguidas = 0;
        motivo = "liga: acima do limite em " + String(CONFIRMACOES) + " leituras";
      } else {
        motivo = "acima do limite, confirmando (" + String(acimaSeguidas) + "/" + String(CONFIRMACOES) + ")";
      }
    } else {
      acimaSeguidas = 0;
      motivo = "normal";
    }
  } else {
    bool cumpriuMinimo = (ts - ligouEm) >= MIN_LIGADO_S || !relogioOk();
    if (podeDesligar && cumpriuMinimo) {
      ventLigado = false;
      motivo = "desliga: voltou abaixo de 27 C e 70 %";
    } else if (podeDesligar) {
      motivo = "mantem ligado: tempo minimo";
    } else {
      motivo = "mantem ligado: ainda acima do limite de desligar";
    }
  }
}

// ================================================================
//  Buffer de leituras (guarda enquanto a internet estiver fora)
// ================================================================
void guardarLeitura(uint32_t ts) {
  if (isnan(tempC) || isnan(umidPct)) return;
  if (nBuf == BUF_MAX) {                     // cheio: descarta a mais antiga
    memmove(&buf[0], &buf[1], sizeof(Leitura) * (BUF_MAX - 1));
    nBuf--; perdidas++;
  }
  Leitura l;
  l.ts = ts;
  l.t10 = (int16_t)lroundf(tempC * 10);
  l.u10 = (uint16_t)lroundf(umidPct * 10);
  l.vent = ventLigado ? 1 : 0;
  l.manual = modoManual ? 1 : 0;
  buf[nBuf++] = l;
}

// ================================================================
//  Rede
// ================================================================
bool conectarWiFi() {
  wifiIniciado = true;
  WiFi.mode(WIFI_STA);
  // Tentativa 1: sem fixar canal (varre). Tentativa 2: canal 6 (onde fica o Wokwi-GUEST).
  // Tentativa 3: varre de novo. No Wokwi a rede simulada as vezes demora.
  for (int tentativa = 1; tentativa <= 3; tentativa++) {
    if (tentativa > 1) {                     // so entre tentativas, e SEM desligar o radio
      WiFi.disconnect(false);
      delay(1000);
    }
    if (tentativa == 2) WiFi.begin(WIFI_SSID, WIFI_PASS, 6);
    else                WiFi.begin(WIFI_SSID, WIFI_PASS);
    Serial.printf("[WiFi] tentativa %d", tentativa);
    unsigned long t0 = millis();
    while (WiFi.status() != WL_CONNECTED && millis() - t0 < 12000) { delay(250); Serial.print("."); }
    if (WiFi.status() == WL_CONNECTED) {
      Serial.printf(" ok | IP=%s (%lu ms)\n", WiFi.localIP().toString().c_str(), (unsigned long)(millis() - t0));
      return true;
    }
    Serial.printf(" falhou (status %d)\n", (int)WiFi.status());
  }
  Serial.println("[WiFi] SEM REDE (sigo decidindo sozinho)");
  return false;
}

void sincronizarRelogio() {
  configTime(0, 0, "pool.ntp.org", "time.nist.gov");   // UTC; reacerta a cada despertar
  Serial.print("[NTP] relogio");
  unsigned long t0 = millis();
  while (!relogioOk() && millis() - t0 < 10000) { delay(250); Serial.print("."); }
  if (relogioOk()) Serial.printf(" ok (%lu)\n", (unsigned long)agora());
  else             Serial.printf(" FALHOU (%lu)\n", (unsigned long)agora());
}

void responder(const String& cmd, uint32_t tsCmd, const String& resultado) {
  String j = "{\"cmd\":\"" + cmd + "\",\"ts_cmd\":" + String(tsCmd) +
             ",\"resultado\":\"" + resultado + "\",\"ventilador\":\"" +
             (ventLigado ? "ligado" : "desligado") + "\",\"modo\":\"" +
             (modoManual ? "manual" : "auto") + "\",\"ts\":" + String(agora()) + "}";
  Serial.println("[CMD] " + resultado);
  mqtt.publish(T_ACK.c_str(), j.c_str());
}

// Comando assinado:  vent:ON|1795018322|<hmac de "vent:ON|1795018322">
// Chega RETIDO no broker: se o ESP32 estava dormindo, ele recebe ao acordar.
void aoReceber(char* topico, byte* payload, unsigned int len) {
  if (len == 0) return;                      // mensagem vazia = limpeza do retido
  String msg;
  for (unsigned int i = 0; i < len; i++) msg += (char)payload[i];

  // Confirmacao da ponte: {"ts":1795456615,"ok":true}
  if (T_PONTE_OK == topico) {
    int p = msg.indexOf("\"ts\":");
    uint32_t ts = p >= 0 ? strtoul(msg.c_str() + p + 5, NULL, 10) : 0;
    if (ts != 0 && ts == tsPonteEsperado && msg.indexOf("true") >= 0) ponteConfirmada = true;
    return;
  }
  Serial.printf("[MQTT] recebido em %s: %s\n", topico, msg.c_str());

  int p1 = msg.indexOf('|'), p2 = msg.lastIndexOf('|');
  if (p1 < 0 || p2 <= p1) {
    responder(msg.substring(0, 20), 0, "RECUSADO: sem assinatura");
    mqtt.publish(T_CMD.c_str(), "", true);   // apaga o lixo retido
    return;
  }
  String cmd = msg.substring(0, p1), tsTxt = msg.substring(p1 + 1, p2), sig = msg.substring(p2 + 1);
  sig.toLowerCase();
  uint32_t ts = strtoul(tsTxt.c_str(), NULL, 10);

  mqtt.publish(T_CMD.c_str(), "", true);     // tira do broker para nao repetir

  if (!iguais(sig, hmacHex(cmd + "|" + tsTxt))) { responder(cmd, ts, "RECUSADO: assinatura invalida"); return; }
  uint32_t t = agora();
  if (ts <= ultimoTsCmd)                      { responder(cmd, ts, "RECUSADO: replay (comando repetido)"); return; }
  if (ts + JANELA_CMD_S < t || ts > t + 60)  { responder(cmd, ts, "RECUSADO: comando expirado"); return; }
  ultimoTsCmd = ts;

  if (cmd == "vent:ON" || cmd == "vent:OFF") {
    modoManual = true;
    manualAte  = t + MANUAL_DURA_S;
    ventLigado = (cmd == "vent:ON");
    if (ventLigado) ligouEm = t;
    aplicarVentilador();
    motivo = "manual (painel)";
    responder(cmd, ts, "OK: " + cmd + " (manual por " + String(MANUAL_DURA_S / 60) + " min)");
  } else if (cmd == "modo:AUTO") {
    modoManual = false;
    decidir(t);
    aplicarVentilador();
    responder(cmd, ts, "OK: modo automatico");
  } else {
    responder(cmd, ts, "RECUSADO: comando desconhecido");
  }
}

bool conectarMqtt() {
  tlsMqtt.setCACert(CA_MOSQUITTO);           // Camada 1: confere quem e' o broker
  tlsMqtt.setHandshakeTimeout(15);
  mqtt.setServer(MQTT_HOST, MQTT_PORTA);
  mqtt.setBufferSize(5120);                  // cabe o lote cheio (48 leituras) da ponte
  mqtt.setKeepAlive(120);
  mqtt.setCallback(aoReceber);

  const char* user = strlen(MQTT_USER) ? MQTT_USER : NULL;
  const char* pass = strlen(MQTT_PASS) ? MQTT_PASS : NULL;
  String willMsg = "{\"status\":\"offline\"}";   // "testamento": se cair sem avisar, o broker publica isso

  for (int tentativa = 1; tentativa <= 3; tentativa++) {
    Serial.printf("[MQTT] %s:%d (TLS)...", MQTT_HOST, MQTT_PORTA);
    String id = String("esp32-") + GRUPO + "-" + LOCAL + "-" + String(esp_random() % 10000);
    if (mqtt.connect(id.c_str(), user, pass, T_ESTADO.c_str(), 1, true, willMsg.c_str())) {
      Serial.println(" ok");
      mqtt.subscribe(T_CMD.c_str(), 1);
      mqtt.subscribe(T_PONTE_OK.c_str(), 1);
      return true;
    }
    char erro[120];
    tlsMqtt.lastError(erro, sizeof(erro));
    Serial.printf(" falhou rc=%d | TLS: %s\n", mqtt.state(), erro);
    delay(2000);
  }
  return false;
}

void publicarEstado(const char* status, uint32_t acordaEm) {
  String j = "{\"status\":\"" + String(status) + "\",\"ventilador\":\"" +
             (ventLigado ? "ligado" : "desligado") + "\",\"modo\":\"" +
             (modoManual ? "manual" : "auto") + "\",\"acorda_em\":" + String(acordaEm) +
             ",\"intervalo_s\":" + String(DORME_S) + ",\"ts\":" + String(agora()) + "}";
  mqtt.publish(T_ESTADO.c_str(), j.c_str(), true);   // RETIDO: o painel ve o ultimo estado na hora
}

void publicarTelemetria() {
  String j = "{\"ts\":" + String(agora()) +
             ",\"temp_c\":" + (isnan(tempC) ? String("null") : String(tempC, 1)) +
             ",\"umid_pct\":" + (isnan(umidPct) ? String("null") : String(umidPct, 1)) +
             ",\"ventilador\":\"" + (ventLigado ? "ligado" : "desligado") + "\"" +
             ",\"modo\":\"" + (modoManual ? "manual" : "auto") + "\"" +
             ",\"motivo\":\"" + motivo + "\"" +
             ",\"bateria_pct\":" + String(bateriaPct) +
             ",\"ciclo\":" + String(ciclos) +
             ",\"pendentes\":" + String(nBuf) + "}";
  bool ok = mqtt.publish(T_TELEM.c_str(), j.c_str());
  Serial.println("[MQTT] telemetria " + String(ok ? "ok: " : "FALHOU: ") + j);
}

void alertar(const char* tipo, const String& texto, uint32_t& ultimo) {
  uint32_t t = agora();
  if (ultimo != 0 && t - ultimo < COOLDOWN_ALERTA_S) {
    Serial.printf("[ALERTA] %s em cooldown (faltam %lu s)\n", tipo, (unsigned long)(COOLDOWN_ALERTA_S - (t - ultimo)));
    return;
  }
  String j = "{\"tipo\":\"" + String(tipo) + "\",\"msg\":\"" + texto + "\",\"ts\":" + String(t) + "}";
  if (mqtt.publish(T_ALERTA.c_str(), j.c_str())) { ultimo = t; Serial.println("[ALERTA] enviado: " + j); }
}

void verificarAlertas() {
  if (!isnan(tempC) && tempC >= T_CRITICA_C)
    alertar("temp_critica", "Temperatura " + String(tempC, 1) + " C no silo", ultAlertaTemp);
  if (falhasSensor >= 2)
    alertar("sensor_falha", "Sensor sem leitura ha " + String(falhasSensor) + " ciclos", ultAlertaSensor);
  if (bateriaPct < BATERIA_FRACA_PCT)
    alertar("bateria_fraca", "Bateria em " + String(bateriaPct) + " %", ultAlertaBateria);
}

// Corpo do lote: o ts (anti-replay) e' sempre o de AGORA
String montarCorpo(uint32_t ts) {
  String corpo = "{\"grupo\":\"" + String(GRUPO) + "\",\"local\":\"" + String(LOCAL) +
                 "\",\"ts\":" + String(ts) + ",\"leituras\":[";
  for (int i = 0; i < nBuf; i++) {
    if (i) corpo += ",";
    corpo += "{\"t\":" + String(buf[i].ts) + ",\"temp_c\":" + String(buf[i].t10 / 10.0, 1) +
             ",\"umid_pct\":" + String(buf[i].u10 / 10.0, 1) + ",\"vent\":" + String(buf[i].vent) +
             ",\"manual\":" + String(buf[i].manual) + "}";
  }
  corpo += "]}";
  return corpo;
}

// Caminho 1: POST direto no Worker (Camadas 1, 2 e 3). Devolve true se gravou.
bool enviarHistorico() {
  if (nBuf == 0) return true;
  // No Wokwi o handshake TLS com a Cloudflare (certificado ECDSA P-384) leva
  // ~13 s em PC lento e a Cloudflare desiste ("SSL - EOF"). No demo tenta 1x
  // e, se falhar, usa a PONTE pelo MQTT (enviarPelaPonte).
  const int TENTATIVAS = MODO_DEMO ? 1 : 3;
  for (int tentativa = 1; tentativa <= TENTATIVAS; tentativa++) {
    String corpo = montarCorpo(agora());

    unsigned long t0 = millis();
    WiFiClientSecure tls;
    tls.setCACert(CA_WORKER);                  // Camada 1 (nunca setInsecure)
    tls.setHandshakeTimeout(30);
    HTTPClient http;
    http.setConnectTimeout(15000);
    http.setTimeout(15000);
    http.begin(tls, String(WORKER_URL) + "/insert");
    http.addHeader("Content-Type", "application/json");
    http.addHeader("X-API-Key", API_KEY);              // Camada 2
    http.addHeader("X-Assinatura", hmacHex(corpo));    // Camada 3 (o ts esta DENTRO do corpo)
    int codigo = http.POST(corpo);
    String resp = http.getString();
    http.end();

    Serial.printf("[HTTPS] tentativa %d: %d leitura(s) -> %d %s (%lu ms)\n",
                  tentativa, nBuf, codigo, resp.c_str(), (unsigned long)(millis() - t0));
    if (codigo == 200) { nBuf = 0; return true; }  // so apaga do buffer depois de confirmado
    if (codigo > 0) return false;              // o Worker respondeu e recusou (401/400): repetir nao adianta
    char erro[120]; tls.lastError(erro, sizeof(erro));
    Serial.printf("        erro TLS: %s\n", erro);
    if (tentativa < TENTATIVAS) delay(1000);
  }
  return false;
}

// ================================================================
//  Caminho 2 (PONTE): o ESP32 publica o lote JA ASSINADO no MQTT e o painel
//  repassa ao Worker. O painel nao consegue alterar nada: mudou 1 byte, o
//  HMAC nao confere e o Worker recusa. O buffer so e' apagado quando o painel
//  confirma em .../ponte_ok que o Worker respondeu 200.
//  Mensagem em .../ponte:  <assinatura>\n<corpo>
// ================================================================
void enviarPelaPonte() {
  if (nBuf == 0 || !mqtt.connected() || !relogioOk()) return;
  uint32_t ts = agora();
  String corpo = montarCorpo(ts);
  String msg = hmacHex(corpo) + "\n" + corpo;
  tsPonteEsperado = ts;
  ponteConfirmada = false;
  if (!mqtt.publish(T_PONTE.c_str(), msg.c_str())) {
    Serial.println("[PONTE] FALHOU ao publicar (mensagem grande demais?)");
    return;
  }
  Serial.printf("[PONTE] %d leitura(s) assinadas publicadas em .../ponte, esperando o painel...\n", nBuf);
  unsigned long t0 = millis();
  while (!ponteConfirmada && millis() - t0 < 8000) { mqtt.loop(); delay(10); }
  if (ponteConfirmada) {
    Serial.printf("[PONTE] painel confirmou: Worker gravou %d leitura(s) (%lu ms)\n", nBuf, (unsigned long)(millis() - t0));
    nBuf = 0;
  } else {
    Serial.println("[PONTE] sem confirmacao (painel fechado?): leituras ficam no buffer");
  }
}

// ================================================================
//  Energia: mostra no Serial a conta da Etapa 8 com o tempo MEDIDO
// ================================================================
void relatorioEnergia(uint32_t msAcordado) {
  msAcordadoTotal += msAcordado;
  double mediaS = (msAcordadoTotal / (double)ciclos) / 1000.0;
  double periodo = mediaS + DORME_PRODUTO_S;
  double iMedia = (I_ACORDADO_MA * mediaS + I_DORMINDO_MA * DORME_PRODUTO_S) / periodo;
  double horas = (BATERIA_MAH * BATERIA_UTIL) / iMedia;
  Serial.printf("[ENERGIA] ciclo %lu | acordado agora %lu ms | media %.1f s\n",
                (unsigned long)ciclos, (unsigned long)msAcordado, mediaS);
  Serial.printf("[ENERGIA] produto (dorme %d s): corrente media %.2f mA -> autonomia %.0f h (%.0f dias)\n",
                DORME_PRODUTO_S, iMedia, horas, horas / 24.0);
  Serial.printf("[ENERGIA] sem deep sleep: %.0f h (%.1f dias)\n",
                (BATERIA_MAH * BATERIA_UTIL) / I_ACORDADO_MA, (BATERIA_MAH * BATERIA_UTIL) / I_ACORDADO_MA / 24.0);
}

// ================================================================
//  Dormir
//  Produto (MODO_DEMO 0): deep sleep de verdade. O chip desliga, a RAM e'
//    apagada, so a memoria RTC sobrevive, e ele recomeca no setup().
//  Demo (MODO_DEMO 1): o Wokwi nao consegue sair do deep sleep (reinicia pelo
//    watchdog: rst:0x8 TG1WDT_SYS_RESET), entao o sono e' SIMULADO: Wi-Fi
//    desligado + delay(). O ciclo e a medicao do tempo acordado sao os mesmos.
// ================================================================
#ifndef USAR_DEEP_SLEEP
  #define USAR_DEEP_SLEEP (!MODO_DEMO)
#endif

unsigned long inicioCiclo = 0;

void dormir() {
  uint32_t s = (bateriaPct < BATERIA_FRACA_PCT) ? DORME_ECONOMIA_S : DORME_S;
  if (temRede && mqtt.connected()) {
    publicarEstado("dormindo", relogioOk() ? agora() + s : 0);
    for (int i = 0; i < 5; i++) { mqtt.loop(); delay(20); }
    mqtt.disconnect();
  }
  if (wifiIniciado) {                        // so desliga o Wi-Fi se ele chegou a ser ligado
    WiFi.disconnect(true);
    WiFi.mode(WIFI_OFF);
    wifiIniciado = false;
  }

  relatorioEnergia(millis() - inicioCiclo);  // tempo acordado SO deste ciclo
  Serial.printf("[SONO] ventilador %s | dormindo %lu s%s\n\n",
                ventLigado ? "LIGADO" : "desligado", (unsigned long)s,
                USAR_DEEP_SLEEP ? " (deep sleep)" : " (sono simulado no Wokwi)");
  Serial.flush();

#if USAR_DEEP_SLEEP
  #if SEGURAR_RELE_NO_SONO
  gpio_hold_en((gpio_num_t)PINO_RELE);       // ESP32 real: "congela" o pino do rele
  gpio_hold_en((gpio_num_t)PINO_LED);
  gpio_deep_sleep_hold_en();
  #endif
  esp_sleep_enable_timer_wakeup((uint64_t)s * 1000000ULL);
  esp_deep_sleep_start();                    // nao volta daqui: recomeca no setup()
#else
  delay((unsigned long)s * 1000UL);          // volta e o loop() roda o proximo ciclo
#endif
}

// ================================================================
//  Um ciclo: sentir -> decidir/agir -> comunicar
// ================================================================
void ciclo() {
  inicioCiclo = millis();
  bool primeiro = (ciclos == 0);
  ciclos++;
  const char* despertar;
  if (USAR_DEEP_SLEEP) despertar = esp_sleep_get_wakeup_cause() == ESP_SLEEP_WAKEUP_TIMER ? "timer" : "liga/reset";
  else                 despertar = primeiro ? "liga/reset" : "timer (sono simulado)";
  Serial.printf("\n===== Silo %s | ciclo %lu | despertar: %s =====\n", LOCAL, (unsigned long)ciclos, despertar);

  // ---- 1) SENTIR ----
  tempC   = dht.readTemperature();
  umidPct = dht.readHumidity();
  bateriaPct = map(analogRead(PINO_BATERIA), 0, 4095, 0, 100);
  if (isnan(tempC) || isnan(umidPct)) Serial.println("[DHT] FALHA NA LEITURA (NaN)");
  else Serial.printf("[DHT] %.1f C | %.1f %% | bateria %d %%\n", tempC, umidPct, bateriaPct);

  // ---- 2) DECIDIR (na borda, sem depender da rede) e AGIR ----
  decidir(agora());
  aplicarVentilador();
  Serial.printf("[REGRA] ventilador %s | modo %s | %s\n", ventLigado ? "LIGADO" : "desligado",
                modoManual ? "manual" : "auto", motivo.c_str());
  guardarLeitura(relogioOk() ? agora() : 0);

  // ---- 3) COMUNICAR (se der) ----
  if (digitalRead(PINO_SEM_NET) == LOW) {
    Serial.printf("[REDE] chave de teste em SEM INTERNET | %d leitura(s) guardada(s) no buffer\n", nBuf);
    temRede = false;
  } else {
    temRede = conectarWiFi();
  }
  if (temRede) {
    sincronizarRelogio();
    if (relogioOk()) for (int i = 0; i < nBuf; i++) if (buf[i].ts == 0) buf[i].ts = agora();
    // 1o o HTTPS, SOZINHO: duas conexoes TLS abertas ao mesmo tempo (MQTT + HTTPS)
    // podem esgotar a memoria do ESP32 (no Wokwi: "SSL - EOF" ou travamento).
    Serial.printf("[MEM] livre antes do HTTPS: %u bytes\n", (unsigned)ESP.getFreeHeap());
    bool gravou = false;
    if (relogioOk()) gravou = enviarHistorico();
    else Serial.println("[HTTPS] sem relogio certo: nao da pra assinar com ts (fica no buffer)");

    // 2o o MQTT: estado, comando retido, telemetria e alertas
    if (conectarMqtt()) {
      publicarEstado("online", 0);
      unsigned long t0 = millis();           // espera ~2 s: comando retido chega aqui
      while (millis() - t0 < 2000) { mqtt.loop(); delay(10); }
      publicarTelemetria();
      verificarAlertas();
      if (!gravou) enviarPelaPonte();        // plano B: o painel repassa o lote assinado
      t0 = millis();
      while (millis() - t0 < 500) { mqtt.loop(); delay(10); }
    }
  }
  if (perdidas) Serial.printf("[BUFFER] %lu leitura(s) descartada(s) por falta de espaco\n", (unsigned long)perdidas);
}

void setup() {
  Serial.begin(115200);
  delay(50);
  pinMode(PINO_RELE, OUTPUT);
  pinMode(PINO_LED, OUTPUT);
  pinMode(PINO_SEM_NET, INPUT_PULLUP);
  aplicarVentilador();                       // reaplica o estado guardado na RTC
  dht.begin();
  analogReadResolution(12);
  delay(100);
}

// Produto: roda uma vez e o deep sleep reinicia o chip.
// Demo: roda ciclo apos ciclo, com o sono simulado no meio.
void loop() {
  ciclo();
  dormir();
}
