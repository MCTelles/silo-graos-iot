-- ================================================================
--  Rode no console do D1 (Cloudflare -> Storage & Databases -> D1 ->
--  seu banco -> Console). Pode rodar mais de uma vez: "IF NOT EXISTS".
-- ================================================================

-- Uma linha por leitura do silo (o historico)
CREATE TABLE IF NOT EXISTS silo_leituras (
  id          INTEGER PRIMARY KEY AUTOINCREMENT,
  local       TEXT    NOT NULL,          -- ex.: silo1
  ts          INTEGER NOT NULL,          -- hora da LEITURA (segundos UTC, vem do ESP32)
  temp_c      REAL    NOT NULL,
  umid_pct    REAL    NOT NULL,
  ventilador  INTEGER NOT NULL,          -- 1 = ligado, 0 = desligado
  manual      INTEGER NOT NULL,          -- 1 = comando do painel, 0 = automatico
  recebido_em INTEGER NOT NULL           -- hora em que chegou no Worker (segundos UTC)
);
CREATE INDEX IF NOT EXISTS idx_silo_local_ts ON silo_leituras (local, ts);

-- Toda requisicao recusada fica registrada aqui (o "alarme" do sistema)
CREATE TABLE IF NOT EXISTS tentativas (
  id        INTEGER PRIMARY KEY AUTOINCREMENT,
  motivo    TEXT    NOT NULL,
  ip        TEXT,
  timestamp INTEGER NOT NULL
);

-- Assinaturas ja aceitas: se a mesma chegar de novo, e' replay
CREATE TABLE IF NOT EXISTS assinaturas_usadas (
  assinatura TEXT    PRIMARY KEY,
  timestamp  INTEGER NOT NULL
);

-- ================================================================
--  CONSULTA DE RESUMO (Etapa 5) - a mesma que o Worker roda em /resumo
--  Pergunta: "Em cada hora das ultimas 24 h, qual foi a temperatura
--  maxima do silo, a umidade media e quanto tempo o ventilador ficou ligado?"
--  (horario de Brasilia = UTC-3)
-- ================================================================
-- SELECT strftime('%Y-%m-%d %H:00', ts, 'unixepoch', '-3 hours') AS hora,
--        ROUND(MAX(temp_c), 1)          AS temp_max_c,
--        ROUND(AVG(temp_c), 1)          AS temp_media_c,
--        ROUND(AVG(umid_pct), 1)        AS umid_media_pct,
--        ROUND(100.0 * AVG(ventilador)) AS vent_ligado_pct,
--        COUNT(*)                       AS leituras
--   FROM silo_leituras
--  WHERE local = 'silo1' AND ts >= unixepoch() - 24 * 3600
--  GROUP BY hora
--  ORDER BY hora;
