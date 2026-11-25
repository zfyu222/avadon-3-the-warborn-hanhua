param(
    [ValidateSet('Install','Uninstall')] [string] $Action = 'Install',
    [string] $GameRoot = (Split-Path -Parent $PSScriptRoot)
)
$ErrorActionPreference = 'Stop'

function Get-Sha([string] $Path) {
    if (![IO.File]::Exists($Path)) { return '' }
    $stream = [IO.File]::OpenRead($Path)
    $algorithm = [Security.Cryptography.SHA256]::Create()
    try { return [BitConverter]::ToString($algorithm.ComputeHash($stream)).Replace('-', '').ToLowerInvariant() }
    finally { $algorithm.Dispose(); $stream.Dispose() }
}

function Copy-Verified([string] $From, [string] $To, [string] $Expected) {
    if ((Get-Sha $From) -ne $Expected) { throw "File failed verification: $From" }
    if ((Get-Sha $To) -eq $Expected) { return }
    $temporary = Join-Path (Split-Path -Parent $To) ('.av3-' + [Guid]::NewGuid().ToString('N') + '.tmp')
    try {
        [IO.File]::Copy($From, $temporary)
        if ((Get-Sha $temporary) -ne $Expected) { throw "Copy failed verification: $To" }
        if ([IO.File]::Exists($To)) { [IO.File]::Replace($temporary, $To, [NullString]::Value) }
        else { [IO.File]::Move($temporary, $To) }
        if ((Get-Sha $To) -ne $Expected) { throw "Installed file failed verification: $To" }
    }
    finally {
        if ([IO.File]::Exists($temporary)) { [IO.File]::Delete($temporary) }
    }
}

try {
    $root = (Resolve-Path -LiteralPath $GameRoot).ProviderPath
    $manifest = Get-Content -LiteralPath (Join-Path $PSScriptRoot 'manifest.json') -Raw -Encoding UTF8 | ConvertFrom-Json
    $exe = Join-Path $root 'Avadon 3.exe'
    $registry = Join-Path $root 'Avadon3Chinese\resources.bin'
    $target = Join-Path $root 'winmm.dll'
    if ((Get-Sha $exe) -ne $manifest.game_sha256 -or (Get-Sha $registry) -ne $manifest.registry_sha256) {
        throw 'Install the matching Avadon 3 Chinese patch first (Steam Windows build 4248987).'
    }
    foreach ($process in (Get-Process -Name 'Avadon 3' -ErrorAction SilentlyContinue)) {
        if (!$process.Path -or [String]::Equals($process.Path, $exe, [StringComparison]::OrdinalIgnoreCase)) {
            throw 'Close Avadon 3 before applying this change.'
        }
    }
    $backupDirectory = Join-Path $root 'Avadon3Chinese\EnhancementBackup'
    $backup = Join-Path $backupDirectory 'winmm.dll'
    $current = Get-Sha $target
    if ($Action -eq 'Uninstall') {
        if ($current -eq $manifest.baseline_dll_sha256) { Write-Output 'Base Chinese patch is active.'; exit 0 }
        if ($current -ne $manifest.enhanced_dll_sha256) { throw 'The installed game adapter has an unexpected version.' }
        Copy-Verified $backup $target $manifest.baseline_dll_sha256
        Write-Output 'Base Chinese patch restored. Enhancement settings have been saved for reuse.'
        exit 0
    }
    if ($current -ne $manifest.baseline_dll_sha256 -and $current -ne $manifest.enhanced_dll_sha256) {
        throw 'The installed game adapter has an unexpected version.'
    }
    $payload = Join-Path $PSScriptRoot 'winmm.dll'
    $defaults = Join-Path $PSScriptRoot 'defaults.ini'
    $settingsApp = Join-Path (Split-Path -Parent $PSScriptRoot) 'Enhancement-Settings.exe'
    if ((Get-Sha $payload) -ne $manifest.enhanced_dll_sha256 -or (Get-Sha $defaults) -ne $manifest.defaults_sha256 -or
        (Get-Sha $settingsApp) -ne $manifest.settings_sha256) { throw 'Enhancement package failed checksum verification.' }
    $null = [IO.Directory]::CreateDirectory($backupDirectory)
    if (![IO.File]::Exists($backup)) { Copy-Verified $target $backup $manifest.baseline_dll_sha256 }
    $backupHash = Get-Sha $backup
    if ($current -eq $manifest.baseline_dll_sha256 -and
        $backupHash -in @($manifest.upgrade_baseline_dll_sha256)) {
        Copy-Verified $target $backup $manifest.baseline_dll_sha256
    }
    if ((Get-Sha $backup) -ne $manifest.baseline_dll_sha256) { throw 'Existing base adapter backup failed verification.' }
    Copy-Verified $settingsApp (Join-Path $root 'Enhancement-Settings.exe') $manifest.settings_sha256
    $settings = Join-Path $root 'Avadon3Chinese\enhancement.ini'
    if (![IO.File]::Exists($settings)) { Copy-Verified $defaults $settings $manifest.defaults_sha256 }
    Copy-Verified $payload $target $manifest.enhanced_dll_sha256
    Write-Output 'Avadon 3 enhancement installed. Settings take effect the next time the game starts.'
}
catch {
    [Console]::Error.WriteLine($_.Exception.Message)
    exit 1
}
