[CmdletBinding()]
param(
    [string]$Qemu = 'qemu-system-x86_64',
    [string]$Bios = (Join-Path $PSScriptRoot '..\out\bios.bin'),
    [string]$Nvram = (Join-Path $PSScriptRoot '..\out\nvram.bin'),
    [string]$Memory = '128M',
    [Parameter(ValueFromRemainingArguments = $true)]
    [string[]]$Extra
)

$ErrorActionPreference = 'Stop'
$Bios = (Resolve-Path -LiteralPath $Bios).Path
$BiosSize = (Get-Item -LiteralPath $Bios).Length
if ($BiosSize -ne 262144) {
    throw "BIOS image must be exactly 262144 bytes (got $BiosSize)."
}

if (-not (Test-Path -LiteralPath $Nvram)) {
    $buf = New-Object byte[] 262144
    for ($i = 0; $i -lt $buf.Length; $i++) {
        $buf[$i] = 0xFF
    }
    [IO.File]::WriteAllBytes($Nvram, $buf)
}
$Nvram = (Resolve-Path -LiteralPath $Nvram).Path
$NvramSize = (Get-Item -LiteralPath $Nvram).Length
if ($NvramSize -ne 262144) {
    throw "NVRAM image must be exactly 262144 bytes (got $NvramSize)."
}

$biosQ = $Bios -replace '\\', '/'
$nvramQ = $Nvram -replace '\\', '/'
$qemuArgs = @(
    '-machine', 'pc',
    '-m', $Memory,
    '-drive', "if=pflash,format=raw,readonly=on,file=$biosQ",
    '-drive', "if=pflash,format=raw,file=$nvramQ"
)
if ($Extra) {
    $qemuArgs += $Extra
}

& $Qemu @qemuArgs
