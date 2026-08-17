<#
.SYNOPSIS
    Repackage the Espressif emote-tool MMAP bundle into the esp_mmap_assets
    flashable image for the ESP-VoCat-S31 "assets" partition (0x840000).

.DESCRIPTION
    The hand-rolled converter (H:\espwork\_tmp_repack.py) does three things the
    official emote-tool bundle needs before it can be mounted by
    esp_emote_expression on this board:
      1. Rewrites index.json from the flat emoji array into the full
         {emoji_collection, icon_collection, layout} form the loader expects.
      2. Drops emoji entries whose .eaf file is absent from the bundle (the
         upstream bundle lists smile_static / snigger_10s / yummy_20_s whose
         .eaf blobs are missing).
      3. Renames .eaf files longer than 15 chars to e00.eaf..e09.eaf so they fit
         the fixed 16-byte mmap name field, and remaps the emoji_collection
         "file" pointers accordingly.

    After repacking it runs _tmp_tbl.py and asserts that "index.json" is the
    first table entry (the loader fails to mount if this entry is missing).

.NOTES
    Pure-python; no IDF environment needed.
#>
$ErrorActionPreference = 'Stop'

$repack = 'H:\espwork\_tmp_repack.py'
$tbl     = 'H:\espwork\_tmp_tbl.py'
$outBin  = 'H:\espwork\esp-projects\vocat\emote_assets_mmap.bin'

if (-not (Test-Path $repack)) {
    Write-Error "repack script not found: $repack"
    exit 1
}

Write-Output "===== [$(Get-Date)] PACK EMOTE ASSETS ====="
& python $repack 2>&1 | ForEach-Object { Write-Output $_ }
if ($LASTEXITCODE -ne 0) {
    Write-Error "repack failed (exit $LASTEXITCODE)"
    exit $LASTEXITCODE
}

if (-not (Test-Path $outBin)) {
    Write-Error "output bin not produced: $outBin"
    exit 1
}

# Verify index.json is present as the first table entry.
$tblOut = & python $tbl 2>&1
$tblOut | ForEach-Object { Write-Output $_ }
$firstEntry = ($tblOut | Select-Object -Skip 1 | Select-Object -First 1)
if ($firstEntry -notmatch '^\s*0\s+index\.json') {
    Write-Error "VERIFY FAILED: index.json is not the first entry in the image. Loader will fail to mount.`n$firstEntry"
    exit 1
}
Write-Output "VERIFY OK: index.json present as first entry -> emote mount will succeed."

$size = (Get-Item $outBin).Length
Write-Output "Emote assets ready: $outBin ($([math]::Round($size/1MB, 2)) MiB)"
Write-Output "Flash with: esptool_flash.ps1 (writes to 0x840000 -> assets partition)"
