$pyEnv = 'C:\Espressif\python_env\idf6.2_py3.11_env'
$idfPath = 'C:\esp-idf-master'
$proj = 'H:\espwork\esp-projects\vocat-xiaozhi'
$port = 'COM12'
$log = Join-Path $proj 'esptool_flash.log'

# activate.py --export is broken here; set the env by hand (esptool runs via the
# python module, which needs IDF_PATH + the python Scripts on PATH).
$env:IDF_PATH            = $idfPath
$env:IDF_TOOLS_PATH      = 'C:\Espressif\tools'
$env:IDF_COMPONENT_MANAGER = '1'
$env:ESP_IDF_VERSION     = '6.2.0'
$env:PYTHONUTF8          = '1'
$env:PATH = (@("$pyEnv\Scripts") + @($env:PATH)) -join ';'

Set-Location $proj
$flashArgs = @(
    "$pyEnv\Scripts\python.exe",
    '-m','esptool',
    '--chip','esp32s31','-p',$port,'-b','921600',
    '--before','default_reset','--after','hard_reset','--no-stub',
    'write_flash','--flash-mode','dio','--flash-size','16MB','--flash-freq','80m',
    '0x2000','build\bootloader\bootloader.bin',
    '0x8000','build\partition_table\partition-table.bin',
    '0x19000','build\ota_data_initial.bin',
    '0x1c000','build\srmodels\srmodels.bin',
    '0x100000','build\xiaozhi_chat.bin'
)

# Assets partition: the official emote asset pack (pre-rendered 25fps animation
# frames, converted from the Espressif emote-tool MMAP bundle into the
# esp_mmap_assets flashable format). The animated face reads these directly.
$assetsBin = 'H:\espwork\esp-projects\vocat\emote_assets_mmap.bin'
if (-not (Test-Path $assetsBin)) {
    $assetsBin = '..\vocat\emote_assets_mmap.bin'
}
$flashArgs += @('0x840000', $assetsBin)
Write-Output "===== [$(Get-Date)] ESPTOOL FLASH =====" | Tee-Object -FilePath $log -Append
& $flashArgs[0] $flashArgs[1..($flashArgs.Length-1)] 2>&1 | Tee-Object -FilePath $log -Append
Write-Output "ESPTOOL FLASH EXIT: $LASTEXITCODE" | Tee-Object -FilePath $log -Append
