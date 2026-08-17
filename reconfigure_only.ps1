$ErrorActionPreference = 'Continue'

$idfPath = 'C:\esp-idf-master'
$pyEnv   = 'C:\Espressif\python_env\idf6.2_py3.11_env'
$proj    = 'H:\espwork\esp-projects\vocat-xiaozhi'
$log     = Join-Path $proj 'reconfigure_only.log'

$activateOut = & "$pyEnv\Scripts\python.exe" "$idfPath\tools\activate.py" --export 2>&1
$tempScript = ($activateOut | Select-Object -Last 1).Trim()
. $tempScript

$env:IDF_COMPONENT_MANAGER = '1'
$env:SDKCONFIG_DEFAULTS    = "$proj\sdkconfig.defaults;$proj\components\gen_bmgr_codes\board_manager.defaults"

Set-Location $proj

Write-Output "===== [$(Get-Date)] RECONFIGURE =====" | Tee-Object -FilePath $log
& "$pyEnv\Scripts\python.exe" "$idfPath\tools\idf.py" reconfigure 2>&1 | Tee-Object -FilePath $log -Append
Write-Output "RECONFIGURE EXIT: $LASTEXITCODE" | Tee-Object -FilePath $log -Append
