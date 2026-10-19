$ErrorActionPreference = 'Stop'
$installer = $PSScriptRoot
$root = Split-Path $installer -Parent
$src = Join-Path $root 'mmsstv\mmsstv-master-yoniq'
$redist = Join-Path $installer 'redist'
$built = Join-Path $root 'tci-mmw'
$stage = Join-Path $installer 'stage'
if (Test-Path $stage) { Remove-Item $stage -Recurse -Force }
New-Item -ItemType Directory -Path $stage, (Join-Path $stage 'Stock') | Out-Null

function Copy-As($from, $name) {
    if (-not (Test-Path $from)) { throw "Missing $from" }
    Copy-Item $from (Join-Path $stage $name)
}

Copy-As (Join-Path $built 'Yoniq_launch.exe') 'Yoniq.exe'
Copy-As (Join-Path $built 'TCI.mmw') 'TCI.mmw'
Copy-As (Join-Path $redist 'Yoniq_core.exe') 'Yoniq_core.exe'
Copy-As (Join-Path $redist 'MMSSTV.CHM') 'MMSSTV.CHM'
Copy-As (Join-Path $redist 'MMSSTV.HLP') 'MMSSTV.HLP'
Copy-As (Join-Path $src 'ARRL.DX') 'ARRL.DX'
Copy-As (Join-Path $src 'Config.cfg') 'Config.cfg'
Copy-As (Join-Path $src 'def1.mtm') 'DEF1.MTM'
Copy-As (Join-Path $src 'def2.mtm') 'DEF2.MTM'
Copy-As (Join-Path $src 'def3.mtm') 'DEF3.MTM'
Copy-As (Join-Path $src 'def4.mtm') 'DEF4.MTM'
Copy-As (Join-Path $src 'def5.mtm') 'DEF5.MTM'
Copy-As (Join-Path $src 'EMMSSTV.TXT') 'EMMSSTV.TXT'
Copy-As (Join-Path $src 'emstvlog.txt') 'EMSTVLOG.TXT'
Copy-As (Join-Path $src 'EUPDATE.TXT') 'EUPDATE.TXT'
Copy-As (Join-Path $src 'fskid.txt') 'FSKID.TXT'
Copy-As (Join-Path $src 'JASTA.txt') 'JASTA.TXT'
Copy-As (Join-Path $src 'License.TXT') 'License.TXT'
Copy-As (Join-Path $src 'MMCG.DEF') 'MMCG.DEF'
Copy-As (Join-Path $src 'mmsstv.cnt') 'mmsstv.cnt'
Copy-As (Join-Path $src 'mmsstv_i.ico') 'mmsstv_i.ico'
Copy-As (Join-Path $src 'mode.txt') 'MODE.TXT'
Copy-As (Join-Path $src 'NVCG.txt') 'NVCG.TXT'
Copy-As (Join-Path $src 'Readme.txt') 'Readme.txt'
Copy-As (Join-Path $src 'Repeater.txt') 'REPEATER.TXT'
Copy-As (Join-Path $src 'Terms.txt') 'Terms.txt'
Copy-As (Join-Path $src 'COPYING.txt') 'COPYING.txt'
Copy-As (Join-Path $src 'COPYING.LESSER.txt') 'COPYING.LESSER.txt'

$stock = Join-Path $src 'Stock'
foreach ($name in @('Current.bmp','Current.mtm','List.mtm','t1.mtm','t2.mtm','t3.mtm','t4.mtm','t5.mtm')) {
    Copy-Item (Join-Path $stock $name) (Join-Path $stage "Stock\$name")
}
Write-Output "staged $(@(Get-ChildItem $stage -Recurse -File).Count) files"
