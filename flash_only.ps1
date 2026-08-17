$ErrorActionPreference = 'Continue'

$idfPath = 'C:\esp-idf-master'
$pyEnv   = 'C:\Espressif\python_env\idf6.2_py3.11_env'
$proj    = 'H:\espwork\esp-projects\vocat-xiaozhi'
$port    = 'COM12'
$log     = Join-Path $proj 'flash_only.log'

# activate.py --export is broken here; set the env by hand.
$env:IDF_PATH            = $idfPath
$env:IDF_TOOLS_PATH      = 'C:\Espressif\tools'
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION     = '6.2.0'
$env:PYTHONUTF8          = '1'
$env:SDKCONFIG_DEFAULTS  = "$proj\sdkconfig.defaults;$proj\components\gen_bmgr_codes\board_manager.defaults"

Set-Location $proj

Write-Output "===== [$(Get-Date)] FLASH ONLY =====" | Tee-Object -FilePath $log -Append
& "$pyEnv\Scripts\python.exe" "$idfPath\tools\idf.py" flash -p $port -b 921600 2>&1 | Tee-Object -FilePath $log -Append
Write-Output "FLASH EXIT: $LASTEXITCODE" | Tee-Object -FilePath $log -Append
