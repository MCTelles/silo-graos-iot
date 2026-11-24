# ================================================================
#  ATACA o seu Worker do silo (Etapa 7 e Etapa 10 "No ataque")
#
#  Uso (dentro da pasta scripts):
#    powershell -ExecutionPolicy Bypass -File .\ataque_worker.ps1 -Url https://silo-graos.SEU-USUARIO.workers.dev
#
#  Nao precisa de chave nenhuma: e' o que um estranho com a URL faria.
#  Resultado esperado: os dois ataques voltam 401 e NADA e' gravado.
# ================================================================
param([Parameter(Mandatory = $true)][string]$Url)
$Url = $Url.TrimEnd('/')
$ts = [int][double]::Parse((Get-Date -UFormat %s))
$corpo = '{"grupo":"marcelo","local":"silo1","ts":' + $ts + ',"leituras":[{"t":' + $ts + ',"temp_c":20.0,"umid_pct":40.0,"vent":0,"manual":0}]}'

function Tentar($nome, $headers) {
  try {
    $r = Invoke-WebRequest -Uri "$Url/insert" -Method POST -Body $corpo -ContentType "application/json" -Headers $headers -UseBasicParsing
    Write-Host ("{0,-28} -> {1} {2}" -f $nome, $r.StatusCode, $r.Content) -ForegroundColor Red
  } catch {
    $codigo = $_.Exception.Response.StatusCode.value__
    Write-Host ("{0,-28} -> {1} (recusado, como deveria)" -f $nome, $codigo) -ForegroundColor Green
  }
}

Write-Host ""
Write-Host "Atacando $Url/insert com uma leitura FALSA (20 C, 40 %: 'esta tudo bem no silo')" -ForegroundColor Yellow
Write-Host ""
Tentar "1) sem API key"               @{}
Tentar "2) API key chutada + HMAC falso" @{ "X-API-Key" = "chute123"; "X-Assinatura" = ("0" * 64) }
Write-Host ""
Write-Host "Confira no D1 que nenhuma linha 20.0/40.0 apareceu:  $Url/historico?horas=1"
Write-Host ""
