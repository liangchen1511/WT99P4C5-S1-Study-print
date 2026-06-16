# 仅部署家长端静态资源到 ECS 并重启 soti-upload
param(
    [string]$EcsHost = "8.154.20.8",
    [string]$SshUser = "root"
)
$ErrorActionPreference = "Stop"
$Key = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$Local = Join-Path $PSScriptRoot "static\parent"
if (-not (Test-Path $Local)) { throw "缺少 $Local" }
Write-Host ">>> 上传 parent/ 到 ${SshUser}@${EcsHost}:/opt/soti/static/"
ssh -i $Key -o StrictHostKeyChecking=accept-new "${SshUser}@${EcsHost}" "mkdir -p /opt/soti/static/parent"
scp -i $Key -r "$Local\*" "${SshUser}@${EcsHost}:/opt/soti/static/parent/"
Write-Host ">>> 重启 soti-upload"
ssh -i $Key "${SshUser}@${EcsHost}" "systemctl restart soti-upload && sleep 1 && curl -fsS -o /dev/null -w 'GET /parent/ => %{http_code}\n' http://127.0.0.1:3000/parent/"
Write-Host ">>> 公网: http://${EcsHost}/parent/"
