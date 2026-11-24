// ================================================================
//  Copie este arquivo para "config.h" e preencha.
//  O config.h guarda SEGREDOS: ele NUNCA vai pro GitHub
//  (ja esta no .gitignore). Nunca imprima esses valores no Serial.
//
//  ATENCAO (Wokwi): um projeto salvo no Wokwi pode ser aberto por
//  quem tiver o link. NAO compartilhe o link do projeto com o
//  config.h preenchido. Para mostrar ao professor, use os prints.
// ================================================================

// --- Wi-Fi. No Wokwi: "Wokwi-GUEST" e senha "" ---
#define WIFI_SSID  "Wokwi-GUEST"
#define WIFI_PASS  ""

// --- Identidade nos topicos: sis1a/<GRUPO>/<LOCAL>/... ---
#define GRUPO      "marcelo"
#define LOCAL      "silo1"

// --- Seu Worker do silo (sem barra no final) ---
#define WORKER_URL "https://silo-graos.SEU-USUARIO.workers.dev"

// --- Camada 2: API key (o MESMO valor do Secret API_KEY no Worker) ---
#define API_KEY    "troque-por-uma-chave-sua"

// --- Camada 3: chave HMAC (o MESMO valor do Secret CHAVE_HMAC no Worker
//     e do campo "Chave HMAC" no painel) ---
#define CHAVE_HMAC "troque-por-outra-chave-bem-longa"

// --- Broker MQTT com TLS (test.mosquitto.org). A 8885 hoje aceita
//     conexao sem login; deixe usuario e senha vazios. ---
#define MQTT_HOST  "test.mosquitto.org"
#define MQTT_PORTA 8885
#define MQTT_USER  ""
#define MQTT_PASS  ""

// --- Sensor: no Wokwi e' DHT22; na placa do kit, DHT11 ---
#define DHTTYPE    DHT22

// --- 1 = modo demonstracao (dorme 30 s, tempos curtos, bom pro Wokwi)
//     0 = modo produto (dorme 10 min) ---
#define MODO_DEMO  1
