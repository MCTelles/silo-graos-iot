// ================================================================
//  WORKER DO SILO DE GRAOS (Cloudflare Workers + D1)
//  Baseado no Worker seguro da Estacao Cofre.
//
//  Configuracao (Settings -> Variables and Secrets):
//    API_KEY            (Secret)   -> Camada 2
//    CHAVE_HMAC         (Secret)   -> Camada 3
//    EXIGIR_ASSINATURA  (Variable) -> "sim"
//  Binding do D1 com o nome  DB  (Settings -> Bindings -> D1 database).
//
//  Rotas:
//    POST /insert              grava um LOTE de leituras (com todas as travas)
//    GET  /historico?horas=24  leituras para o grafico do painel
//    GET  /resumo?horas=24     resumo por hora (consulta SQL da Etapa 5)
//    GET  /tentativas          ataques recusados (exige X-API-Key)
// ================================================================

const JANELA_S     = 60;                 // a requisicao vale por 60 s (anti-replay)
const GUARDAR_DIAS = 365;                // retencao: uma safra + comparacao com a anterior
const MAX_LOTE     = 100;                // ESP32 manda no maximo 48; folga
const LIMITES      = { temp_c: [-20, 80], umid_pct: [0, 100] };

// O painel e' um arquivo local (file://) -> precisa de CORS nas rotas de leitura
// e no POST /insert (a "ponte": o painel repassa o lote que o ESP32 ja assinou;
// as travas sao as mesmas, entao o navegador nao consegue forjar nada)
const CORS = {
  "Access-Control-Allow-Origin": "*",
  "Access-Control-Allow-Methods": "GET, POST, OPTIONS",
  "Access-Control-Allow-Headers": "Content-Type, X-API-Key, X-Assinatura",
};
const json = (dados, status = 200) =>
  new Response(JSON.stringify(dados), { status, headers: { "Content-Type": "application/json", ...CORS } });

export default {
  async fetch(request, env) {
    const url = new URL(request.url);
    if (request.method === "OPTIONS") return new Response(null, { headers: CORS });

    // ---------------- ESCRITA: passa pelas travas ----------------
    if (url.pathname === "/insert" && request.method === "POST") {
      const corpo = await request.text();           // texto CRU: e' ele que foi assinado
      const barrado = await autenticar(request, env, corpo);
      if (barrado) return barrado;

      let b;
      try { b = JSON.parse(corpo); } catch { return negar(request, env, 400, "JSON invalido"); }

      const local = String(b.local ?? "");
      if (!/^[a-z0-9_]{1,32}$/.test(local)) return negar(request, env, 400, "local invalido");
      if (!Array.isArray(b.leituras) || b.leituras.length === 0 || b.leituras.length > MAX_LOTE)
        return negar(request, env, 400, "lote vazio ou grande demais");

      const agora = Math.floor(Date.now() / 1000);
      const linhas = [];
      for (const l of b.leituras) {
        // validar entrada: nunca confiar no payload, mesmo autenticado
        const temp = Number(l.temp_c), umid = Number(l.umid_pct);
        if (!Number.isFinite(temp) || temp < LIMITES.temp_c[0] || temp > LIMITES.temp_c[1])
          return negar(request, env, 400, "temp_c fora da faixa");
        if (!Number.isFinite(umid) || umid < LIMITES.umid_pct[0] || umid > LIMITES.umid_pct[1])
          return negar(request, env, 400, "umid_pct fora da faixa");
        let t = Number(l.t);
        // leitura guardada offline pode ser antiga (ate 7 dias); sem relogio (0) -> usa a hora de chegada
        if (!Number.isInteger(t) || t <= 0) t = agora;
        if (t > agora + 300 || t < agora - 7 * 86400) return negar(request, env, 400, "hora da leitura invalida");
        linhas.push(env.DB.prepare(
          "INSERT INTO silo_leituras (local, ts, temp_c, umid_pct, ventilador, manual, recebido_em) VALUES (?, ?, ?, ?, ?, ?, ?)"
        ).bind(local, t, temp, umid, l.vent ? 1 : 0, l.manual ? 1 : 0, agora));
      }
      // retencao (LGPD e custo): apaga o que passou do prazo
      linhas.push(env.DB.prepare("DELETE FROM silo_leituras WHERE ts < ?").bind(agora - GUARDAR_DIAS * 86400));
      // IP de quem foi recusado e' dado pessoal: guarda so 30 dias
      linhas.push(env.DB.prepare("DELETE FROM tentativas WHERE timestamp < ?").bind(Date.now() - 30 * 86400 * 1000));
      await env.DB.batch(linhas);                    // tudo ou nada
      return new Response(`OK: ${b.leituras.length} leitura(s) de ${local}`, { headers: CORS });
    }

    // ---------------- LEITURA (aberta: temperatura de grao nao e' dado pessoal) ----------------
    const horas = Math.min(Math.max(parseInt(url.searchParams.get("horas") || "24", 10) || 24, 1), 24 * 31);
    const local = url.searchParams.get("local") || "silo1";
    const desde = Math.floor(Date.now() / 1000) - horas * 3600;

    if (url.pathname === "/historico") {
      const { results } = await env.DB.prepare(
        `SELECT ts, temp_c, umid_pct, ventilador, manual
           FROM silo_leituras WHERE local = ? AND ts >= ? ORDER BY ts LIMIT 2000`
      ).bind(local, desde).all();
      return json(results);
    }

    if (url.pathname === "/resumo") {
      // "Em cada hora, qual foi a temperatura maxima, a umidade media
      //  e quanto tempo o ventilador ficou ligado?"
      const { results } = await env.DB.prepare(
        `SELECT strftime('%Y-%m-%d %H:00', ts, 'unixepoch', '-3 hours') AS hora,
                ROUND(MAX(temp_c), 1)          AS temp_max_c,
                ROUND(AVG(temp_c), 1)          AS temp_media_c,
                ROUND(AVG(umid_pct), 1)        AS umid_media_pct,
                ROUND(100.0 * AVG(ventilador)) AS vent_ligado_pct,
                COUNT(*)                       AS leituras
           FROM silo_leituras
          WHERE local = ? AND ts >= ?
          GROUP BY hora ORDER BY hora`
      ).bind(local, desde).all();
      return json(results);
    }

    // ---------------- ALARME: tentativas recusadas (so o dono ve: tem IP) ----------------
    if (url.pathname === "/tentativas") {
      if (!env.API_KEY || !iguais(request.headers.get("X-API-Key") || "", env.API_KEY))
        return json({ erro: "Unauthorized" }, 401);
      const { results } = await env.DB.prepare(
        "SELECT motivo, ip, timestamp FROM tentativas ORDER BY timestamp DESC LIMIT 50"
      ).all();
      return json(results);
    }

    return new Response("Rotas: POST /insert | GET /historico?horas=24 | GET /resumo?horas=24 | GET /tentativas",
      { status: 404, headers: CORS });
  }
};

// ================================================================
//  autenticar(): devolve null se pode passar, ou a Response de recusa.
//  (igual a Estacao Cofre)
// ================================================================
async function autenticar(request, env, corpo) {
  if (!env.API_KEY) return new Response("Configure o Secret API_KEY no Worker", { status: 500 });

  // Camada 2: API key
  if (!iguais(request.headers.get("X-API-Key") || "", env.API_KEY))
    return negar(request, env, 401, "sem API key ou API key errada");

  if (env.EXIGIR_ASSINATURA !== "sim") return null;
  if (!env.CHAVE_HMAC) return new Response("Configure o Secret CHAVE_HMAC no Worker", { status: 500 });

  // Camada 3: assinatura do corpo inteiro
  const assinatura = (request.headers.get("X-Assinatura") || "").toLowerCase();
  const esperada   = await hmacHex(env.CHAVE_HMAC, corpo);
  if (!iguais(assinatura, esperada))
    return negar(request, env, 401, "assinatura invalida (corpo alterado ou chave errada)");

  // Anti-replay 1: o ts (dentro do corpo assinado) e' de agora?
  let ts;
  try { ts = Number(JSON.parse(corpo).ts); } catch { ts = NaN; }
  const agora = Math.floor(Date.now() / 1000);
  if (!Number.isInteger(ts) || Math.abs(agora - ts) > JANELA_S)
    return negar(request, env, 401, `expirada (ts fora da janela de ${JANELA_S} s)`);

  // Anti-replay 2: essa assinatura ja foi usada?
  const r = await env.DB.prepare(
    "INSERT OR IGNORE INTO assinaturas_usadas (assinatura, timestamp) VALUES (?, ?)"
  ).bind(assinatura, Date.now()).run();
  if (r.meta.changes === 0) return negar(request, env, 401, "replay (assinatura ja usada)");

  await env.DB.prepare("DELETE FROM assinaturas_usadas WHERE timestamp < ?")
    .bind(Date.now() - 5 * 60 * 1000).run();
  return null;
}

async function hmacHex(chave, msg) {
  const enc = new TextEncoder();
  const k = await crypto.subtle.importKey("raw", enc.encode(chave),
    { name: "HMAC", hash: "SHA-256" }, false, ["sign"]);
  const mac = await crypto.subtle.sign("HMAC", k, enc.encode(msg));
  return [...new Uint8Array(mac)].map(b => b.toString(16).padStart(2, "0")).join("");
}

function iguais(a, b) {
  if (a.length !== b.length) return false;
  let dif = 0;
  for (let i = 0; i < a.length; i++) dif |= a.charCodeAt(i) ^ b.charCodeAt(i);
  return dif === 0;
}

async function negar(request, env, status, motivo) {
  try {
    await env.DB.prepare("INSERT INTO tentativas (motivo, ip, timestamp) VALUES (?, ?, ?)")
      .bind(motivo, request.headers.get("CF-Connecting-IP") || "?", Date.now()).run();
  } catch (e) { /* tabela ainda nao criada: segue recusando */ }
  return new Response(`Recusado: ${motivo}`, { status, headers: CORS });
}
