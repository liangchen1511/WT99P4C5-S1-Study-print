# 本机一键部署 SoTi 多模式到阿里云 ECS（A1–A5）
# 用法:
#   .\deploy-to-ecs.ps1
#   .\deploy-to-ecs.ps1 -EcsHost 你的公网IP -SshUser root
# 需已配置 SSH：本机公钥写入 ECS ~/.ssh/authorized_keys，或安装 sshpass 后设 $env:ECS_SSH_PASSWORD

param(
    [string]$EcsHost = "8.154.20.8",
    [string]$SshUser = "root",
    [string]$ProjectRoot = (Resolve-Path (Join-Path $PSScriptRoot "..\..")).Path
)

$ErrorActionPreference = "Stop"
$Key = Join-Path $env:USERPROFILE ".ssh\id_ed25519"
$RemoteOpt = "/opt/soti"
$LocalSrv = Join-Path $ProjectRoot "tools\soti-standalone-server"
$EcsBundle = Join-Path $LocalSrv "ecs"

foreach ($f in @("soti_upload_server.py", "soti_modes.py", "soti_answer_sanitize.py")) {
    $p = Join-Path $LocalSrv $f
    if (-not (Test-Path $p)) { throw "缺少 $p" }
}

Write-Host ">>> 上传 Python 到 ${SshUser}@${EcsHost}:${RemoteOpt}/"
ssh -i $Key -o StrictHostKeyChecking=accept-new "${SshUser}@${EcsHost}" "mkdir -p $RemoteOpt"
scp -i $Key -o StrictHostKeyChecking=accept-new `
    (Join-Path $LocalSrv "soti_upload_server.py") `
    (Join-Path $LocalSrv "soti_modes.py") `
    (Join-Path $LocalSrv "soti_answer_sanitize.py") `
    "${SshUser}@${EcsHost}:${RemoteOpt}/"

Write-Host ">>> 上传 systemd 配置到 /tmp/soti-ecs/"
ssh -i $Key "${SshUser}@${EcsHost}" "rm -rf /tmp/soti-ecs && mkdir -p /tmp/soti-ecs"
scp -i $Key -r "$EcsBundle\*" "${SshUser}@${EcsHost}:/tmp/soti-ecs/"

Write-Host ">>> 远程执行 deploy-on-ecs.sh"
ssh -i $Key "${SshUser}@${EcsHost}" "bash /tmp/soti-ecs/deploy-on-ecs.sh"

Write-Host ""
Write-Host ">>> 公网自检（本机）"
curl.exe -sS "http://${EcsHost}/upload/health"
Write-Host ""
$token = "esp32souti"
curl.exe -sS -H "Authorization: Bearer $token" "http://${EcsHost}/upload/daily"
Write-Host ""
Write-Host "Done. Check health JSON for modes and daily for answer field."
